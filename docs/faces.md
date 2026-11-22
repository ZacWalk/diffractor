# Diffractor Faces

This document owns faces: the people a collection's files name, finding faces in pictures and videos and
comparing them, the face book that keeps what the user tells Diffractor about them, the Faces view, and
saving names to files. [Design](design.md#user-mental-model) owns the targeting and review rules every view
keeps, [file I/O](file-io.md) how a name reaches a file, [implementation](implementation.md#index-search-and-database)
the index, [agents](agents.md#5-the-tools) the agent tools over faces, and [third-party](third-party.md#faces)
libmug, which owns the networks and their pins.

## 1. What faces are for

Faces answer who is in this picture, and where else this face appears, from three kinds of knowledge kept
apart because different parties vouch for them:

- **Names** are what files say: face regions in a picture's XMP, written by a user in Diffractor or another
  organizer. The file is their record ([collections](collections.md#8-truth-staleness-and-honesty)), and
  they are searchable whether or not face search is on.
- **Faces** are what Diffractor finds - each face's box, landmarks and face hash, from the pixels of
  pictures and videos' key frames - once the user turns face search on. All of it is a rebuildable cache.
- **The face book** records whom the user confirmed, whom each face is not, and which faces they hid. It
  lives apart from the index and can be exported.

The user names groups of lookalikes in **Unknown**, then confirms or rules out the **Suggested** faces each
person's page lists. A suggestion never becomes a name by itself, and every confirmation is one more example
suggestions learn from. All of it runs locally, off the UI thread. Saving names to files is not offered in
this release ([§10](#10-saving-names-to-files)).

## 2. People the files name

### 2.1 Reading names

Names are read with the rest of a file's metadata, from its packet or its sidecar, by libfiles
([properties.h](https://github.com/ZacWalk/libfiles/blob/main/include/libfiles/properties.h)):

| Schema | Gives | Read |
| --- | --- | --- |
| The Metadata Working Group's regions: each `mwg-rs:RegionList` item whose `mwg-rs:Type` is `Face` | `mwg-rs:Name`, and the `mwg-rs:Area` it covers | Always. Lightroom, digiKam, Picasa and most organizers write these |
| Microsoft's regions: each `MP:RegionInfo` region | `MPReg:PersonDisplayName`, and its `MPReg:Rectangle` | Only from a packet holding no `mwg-rs` face region. Windows Photo Gallery wrote these |
| IPTC's `Iptc4xmpExt:PersonInImage` | A name, without an area | Always, joining the names the regions give |

A face region without a name still has its area read, so naming a face found in it names that region
rather than adding one. Other region types - `Pet`, `Focus`, `BarCode` - are not faces, and are kept when
Diffractor writes. A name given only as a keyword, in `dc:subject` or as `People|Jane Doe` in
`lr:hierarchicalSubject`, stays a tag: it does not say which face it means. Names compare without regard to
case, and a person is shown as most of their pictures spell them.

Names are a multi-valued property, stored as tags are in the item's cached record. Each region's area is
kept as the file states it - four 16-bit fractions, with the dimensions the file says it measured on - and
is turned upright, or found stale ([§2.2](#22-where-a-region-is)), as the record is read. A region is
therefore placed without reading the file, and a correction to how its space is judged needs nothing read
again.

### 2.2 Where a region is

`stArea:x` and `stArea:y` are an `mwg-rs` area's centre and `stArea:w` and `stArea:h` its size, as
fractions of the picture from its top left; Microsoft's `MPReg:Rectangle` gives left, top, width and height.
The Metadata Working Group's guidelines (2.0, section 5.9) apply regions to the picture as stored, so
Diffractor turns an area upright as it reads it, into the space found faces are kept in
([§4.1](#41-detection)), and back as it writes it.

`mwg-rs:AppliedToDimensions` is the only evidence of which space a writer used, so its shape decides:

- The stored picture's shape, or no dimensions: as stored, as the guidelines say.
- The shape the picture is shown in, a quarter turn away: measured as displayed, so already upright.
- Neither: the picture has been cropped since. Its names are kept and its areas dropped, rather than
  drawn on the wrong part of the picture.

A square picture, or one turned half a turn, cannot tell the first two apart and is read as stored; a
resize, or a crop that kept the proportions, leaves the shape alone, so its areas are read as they stand.
Microsoft's regions carry no dimensions and are read as stored.

### 2.3 Searching for people

`person:` and plain text find people by name ([§8](#8-searching)), even with face search off. With it on,
an explicit correction to a face takes precedence over the name on its region, without erasing another
region naming the same person, a name without a region, or the metadata on disk.

## 3. Face search

### 3.1 Turning it on and off

**Scan for faces**, in the sidebar or the Faces view, turns face search on for photos and videos; it is not
**Find similar faces**, which searches for a selected face. The Faces view's panel and the Faces card in
Options' collection section have a check for each kind, **Scan for faces in photos** and **Scan for faces in
videos**. Turning on a kind not read yet asks first, saying that faces are found and compared on this
machine and nothing is uploaded; how many pictures, videos or both will be read once more (a kind the
collection holds none of goes unmentioned), about how long that takes, and that the scan can be paused; and
that names and review decisions stay on this machine unless exported. The estimate takes each worker
([§3.2](#32-the-scan)) to read five pictures a second and a video in four seconds; once scanning, the Faces
view uses the rate measured.

- **Pause** stops the scan and keeps its results in use, across restarts until resumed; the Faces view and
  the sidebar say so.
- **Off** - both checks cleared - stops the scan and puts its results away, unshown and unloaded, but kept in
  the index, so turning face search on again carries on where it stopped. Clearing one check puts away that
  kind alone. Names and the face book are unaffected.
- **Delete face data**, in the Faces view and Options' Faces card, turns face search off and removes its
  results after a confirmation saying how many pictures' faces go, **Cancel** being the default. Ticking
  **Also forget names and review decisions made in Diffractor** asks again - **Forget names and decisions?**,
  saying how many people and decisions go and that it cannot be undone - also defaulting to **Cancel**;
  otherwise the face book stays, to place its decisions on the faces found again ([§6.2](#62-marks)).
  Deleting acts at once, rewriting the index rows with SQLite's `secure_delete` on, and the face database
  overwrites and compacts what it forgets ([§6.4](#64-the-face-database)), so no face hash is left in free
  space. A window that only shows faces cannot delete them.

Resetting the index removes what face search found and leaves the face book. Turning face search off and
deleting its data invalidate the work in flight first, so nothing begun before either is published after.

### 3.2 The scan

The scan reads a picture or video once its metadata, sidecar included, is current in the index; it never
downloads a file held only in the cloud, and reads a video only where its metadata gives a width and a
height. The picture or video on screen goes next, out of turn; the rest a folder at a time, pictures and
videos together, the folders holding the newest first and each in path order, so reads stay together on
disk and recent pictures come early. A folder that cannot be reached - a disconnected drive, an offline
share - is remembered for the session, until face search is turned on again: its pictures count as waiting
rather than unreadable, and each later pass looks for each such drive or share once before reading there.

One thread reads each picture whole and hands it to a few workers - a quarter of the cores, between one and
four - each with its own libmug detector and embedder; a raw file, or a still too large to read whole, is
loaded by the worker, and a video is read a key frame at a time ([§3.3](#33-videos)). Reads waiting for a
worker are bounded in number and bytes, as the thumbnail pipeline's are
([implementation](implementation.md#thumbnail-pipeline)). File indexing has priority, through an admission
gate the two share ([implementation](implementation.md#synchronized-types)): on a hard drive, a network share
or storage of unknown speed, face reads finish before indexing starts, and the two overlap only where both
are on storage the system reports as non-rotating. Folder discovery and on-demand indexing always go first.

Records are written in batches, so closing loses only the pictures in flight. A picture with no face gets an
empty record, and one that opens but does not decode a record saying so; neither is read again until it
changes. One that cannot be opened for now - locked, or on storage that failed mid-read - gets no record and
is retried on later passes, at most three times a session. A picture the index no longer holds has its faces
put away by the next pass; one the scan merely passes over, such as a cloud-only file, keeps them.

A record is current while it carries the running model's id and detection revision and its stamp equals the
file's modified time; a new detection revision revisits older records without changing the model id or
losing their decisions. A metadata-only write by Diffractor, naming a face among them, leaves the pixels as
they were ([file I/O](file-io.md#coherent-read-back)), so a record current before it is restamped with the
file's new time, whether face search is on or off: at once after a staged write, or from the folder scan
that follows a patch in place, unless half an hour passes first, so a change made elsewhere is never taken
for the write's. A write of the declared orientation alone also turns the record's boxes and landmarks,
keeping its face hashes. Any other change, Rotate and Diffractor's other pixel edits among them, has the
picture read again. A record from another model keeps its boxes until then, but its face hashes are compared
with nothing.

Progress counts every eligible file, so a resumed scan reports where the collection stands. The sidebar shows
**Indexing faces** and a percentage, and opens the Faces view when clicked ([§9.4](#94-the-sidebars-faces));
the Faces view adds how many are left - files rather than pictures where it reads videos - how many could not
be read or wait for their drive, and about how long the rest will take. Results join the store a few times a
second, and the window redraws at that pace.

### 3.3 Videos

A video is read at its key frames alone - frames stored whole, which decode without any before them - so
none is decoded through, and the frame a face was found in can be read again at once to show it. Phones and
action cameras store about one a second, films one every few seconds.

The length is divided into 256 equal parts and the first key frame in each is read, the next read beginning
in the part after the one that frame fell in, so no key frame is read twice and a video whose key frames are
sparser than its parts is read at every one: a phone's clip of up to about four minutes at each key frame, a
film at 256 frames spread through it. A video that cannot seek is read on from its start, and one of unknown
length key frame after key frame, up to 256.

The scan's thread reads the key frames, seeking to each part, while it holds the scan's storage permit, and
hands each to the workers within a picture's bounds. Each is decoded alone and read as a picture
([§4](#4-finding-a-face)), as playback shows it: upright, with square pixels, deinterlaced and HDR tone
mapped. A key frame stored as two fields, as AVCHD and other transport streams hold interlaced video, is read
with its second field and decoded whole.

The faces of all the key frames are then made one record, one face for each person:

- A face is someone already seen where it resembles their clearest face, their first, or one of the last
  four they were seen with by at least 0.6 - or 0.45 where it stands where they stood in the frame read
  before, as someone turning or moving does. Two faces in one frame are two people, and the most alike pairs
  are matched first.
- A person keeps their clearest face - a good face ([§4.4](#44-quality)) first, then the larger, the more
  square-on and the more confident - with its frame's time. A video keeps at most 64 people, the clearest.
- A person keeps the stretches they were seen in, from halfway since the frame read before the first that
  found them to halfway to the frame read after the last; a frame without them between two with them is a
  look away, not an exit.
- A face seen once and unclear, where a frame read on either side lies within a second, is dropped: a
  passing shape is likelier than a person there for one moment.
- A video none of whose key frames decodes, or holding no picture this build decodes, gets a record saying
  so.

A video's record is one picture to the overview ([§7](#7-recognizing-people)), so two of its faces are never
suggested for one person; otherwise its faces are treated as any other. A person's portraits prefer photos.

## 4. Finding a face

```mermaid
flowchart LR
    A[Bounded picture decode] --> B[Luminance plane]
    B --> C[YuNet detection]
    C --> D[Five-point alignment]
    D --> E[SFace embedding]
    E --> F[Face hash]
```

### 4.1 Detection

Detection finds each face in one decoded picture as a box, five landmarks - the eye centres, the nose tip
and the mouth corners - and a confidence percentage. It reads only the pixels it is handed and identifies
nobody. [libmug](https://github.com/ZacWalk/libmug) runs YuNet, libfacedetection's network, on its own
kernels ([design](https://github.com/ZacWalk/libmug/blob/main/docs/design.md#detection)); Diffractor relies
on the following:

- **Input.** libmug reads a decode where it lies: `find_faces` takes packed 32-bit or NV12, a picture's
  record is read from a packed decode, and any other format, P010 among them, finds nothing. The first pass
  reads luminance reduced to at most 640 pixels on the long edge: a smaller plane costs less, but loses
  smaller faces.
- **Upright.** Faces are found and reported in the space the picture is shown in, libmug turning the reduced
  plane upright, since most formats decode in the file's own orientation and the network finds no face lying
  on its side. A face set's extent is the decode's dimensions turned upright, smaller than the file's for a
  bounded decode, so a box is placed by scaling from the extent, never from the file's dimensions.
- **Floors.** The first pass reports faces of 70% confidence or more and at least 3% of the short edge in
  both dimensions, and of two overlapping by more than 45% of their union only the more confident. The
  floors and the plane's edge are one tuning, in [model_faces.h](../src/model_faces.h).
- **Order.** Faces are listed left to right, then top to bottom, then larger first, compared in whole
  pixels, so a picture read again at another size lists them in the same order - the order face terms number
  ([§8](#8-searching)). Boxes and landmarks keep sub-pixel precision, and a box can reach past the edge.
- **A second look.** On a close-up, or with evidence of distant faces, one mirrored pass looks again at
  another scale - for distant faces a larger plane, down to 2% of the short edge, at the same confidence. A
  new face not overlapping one found is measured on the original and appended, renumbering nothing.

### 4.2 Alignment

libmug fits the five landmarks onto the template SFace was trained with by a similarity - rotation, one scale
and translation, never a general affine, whose shear would bend the face - mapping the upright landmarks back
through whatever orientation the decode still carries, and samples the 112-pixel crop exactly as OpenCV's
FaceRecognizerSF does, since SFace's published threshold holds for that input alone
([design](https://github.com/ZacWalk/libmug/blob/main/docs/design.md#recognition)). Diffractor hands it a
decode fitted within 1600 pixels on its long edge, apart from the detector's smaller plane - a JPEG decoded
reduced as far as that allows, 4000 by 3000 at 2000 by 1500, then fitted - and a crop that would sample more
than a quarter outside the picture is refused, not padded: half a face embeds as confidently as a whole one,
and resembles every other half face.

### 4.3 Embedding

OpenCV Zoo's SFace, a MobileFaceNet, turns the crop into 128 values. libmug compiles its weights in,
quantized, and runs one single-threaded instance per worker, so Diffractor owns the parallelism and the
bounds; the weights' storage and accuracy are libmug's
([design](https://github.com/ZacWalk/libmug/blob/main/docs/design.md#the-networks)). The vector is
normalised, so every comparison is a dot product, and quantized into a face hash
([§5.1](#51-the-face-record)). The model id is one number for everything a face hash depends on - libmug's
networks and how it finds, aligns and samples a face, and how Diffractor decodes the picture - and every
record carries it.

### 4.4 Quality

Each face keeps its confidence, its size - the distance between its eyes in the decode - and how square-on
it is, from how evenly the nose sits between the eyes and how level they are. A **good** face is large,
square-on and confident enough to carry an identity, by floors in [model_faces.h](../src/model_faces.h). The
record keeps the measures, not the judgment, so a corrected floor applies to every face with nothing read
again.

Good faces are preferred as portraits, and only good faces form Unknown's groups ([§9.1](#91-people)): the
network finds blurred faces, pets and toys too. Every face with a face hash is still compared, so a blurred
face in a crowd can still be found or suggested. A picture keeps at most 64 faces, the largest of the first
pass.

### 4.5 Named regions and found faces

A found face and a named region are one face when they overlap by enough of their union, or the region
holds most of the face without being large enough to hold a crowd; each is matched once, the largest overlap
first. Matching happens in memory whenever either changes and is never stored, so it cannot disagree with
the file.

A matched face is shown once, under its region's name, and its face hash is one of that person's examples. A
named region the detector missed is searched again, enlarged, through a view of that part of the decode, so a
small or turned face someone named still gets a face hash where one can be found; it follows the detector's
faces in the stable order. A region with no face found is still a name for `person:` and its person's page,
but nothing is compared with it.

## 5. Storage

### 5.1 The face record

A picture's faces are one record in its item's cached record, under the private property
`prop::cache::faces` ([model_property.h](../src/model_property.h)), outside the public property registry.
Metadata writes carry it forward, and only [§3.2](#32-the-scan)'s rules change it
([model_db_pack.h](../src/model_db_pack.h)).

It holds its format, the model id, its stamp, the upright extent its faces were measured on and whether it is
a video's, then each face in the stable order: box and landmarks as 16-bit fractions of the upright picture,
confidence and quality measures, two legacy marks, its face hash and, in a video, its frame's time in
milliseconds. A video's record then lists each person's stretches ([§3.3](#33-videos)), 9 bytes each. A
**face hash** is the normalised vector quantized to 128 signed bytes and one scale: a quarter the size of
floats, typically moving a similarity by under a thousandth. A face costs about 170 bytes; a picture with
none, an empty record saying it was read.

A record describes its own shape: a reader takes the fields it knows, steps over the rest by the stated
stride and leaves unknown sections unread, so a release can add a field or a section without the collection
being read again. A malformed record, or one in a format this build does not read, counts as unscanned.

The legacy marks - hidden, and the names a face was dismissed for - are carried so nothing is lost, never
set: a face hidden so stays hidden where the face book has no mark for it ([§6.2](#62-marks)). A picture read
again with unchanged pixels, by a new model or a corrected detector, carries them to the face found again in
each marked face's place. Only the face search session that made a record writes it, so nothing queued before
face search was turned off or its data deleted is written after.

### 5.2 The face store

Records load at start, in the pass that reads every cached property, while face search is on; otherwise, or
for a kind it does not read ([§3.1](#31-turning-it-on-and-off)), they are stepped over, and turning it on or
changing its kinds loads them in one pass on the database's thread. The face store keeps faces in blocks of
4,096, allocated as it fills and never moved, so a 4-byte number names a face, resolves without a lock and
never changes meaning once published. Face hashes sit apart, contiguous and aligned, so a comparison streams
nothing else; the rest of each face sits in parallel arrays. A picture's faces are consecutive, a run, and
the store maps each picture to its current run.

One writer appends at a time - the load at start, then the scan's thread, which also restamps. Readers see
what it has published, with a generation that moves on every change they could see. A new or restamped
record appends a run and retires the old one, which comparisons skip. A search, a suggestion pass or a value
on screen holds the store it began with, so turning face search off frees it when the last holder goes;
retired runs stay allocated until then, and the next start loads only current ones. A face held costs under
200 bytes, so 300,000 faces take under 60 MB.

### 5.3 Comparing faces

Similarity is the cosine between two faces' vectors: the dot product of their face hashes, scaled, computed
by brute force over the whole store. One face against 300,000 reads about 38 MB, a few milliseconds on one
core, and in 128 dimensions, at the similarities faces are compared by, a search tree would prune almost
nothing. The dot product is written by hand for AVX2, SSE2 and NEON rather than left to the compiler, whose
result would depend on its flags; every path sums the same products exactly in integers and scales them in
the same order, so all agree to the bit.

Two thresholds and a margin, in [model_faces.h](../src/model_faces.h), decide what resembles what:

| Threshold | Decides |
| --- | --- |
| `similar` | The `face:` resemblance search, and the least likeness at which naming a face offers a person ([§9.5](#95-naming-a-face)). It starts at 0.363, SFace's published operating point for one person |
| `confident` | Above `similar`: the least a face nobody has placed must resemble a person to be offered to them ([§7.2](#72-suggestions)) |
| `margin` | 0.05: how much more a face must resemble the person it is offered to than anyone else it could be; naming a face offers those within it of the first |

A threshold trades missed faces against lookalikes; none separates identities. SFace's operating point holds
for adults - two different adults in one photograph rarely reach `similar` - but not for young children,
whose faces a network trained mostly on adults finds rounder and less distinct: two children in one
photograph, siblings most of all, commonly resemble each other more than an adult resembles themself across
the years, though each still resembles their own faces most. So Diffractor compares rather than measures: a
face goes to the person it resembles most, by `margin` over everyone else it could be
([§7.2](#72-suggestions)), and two faces of one picture are never suggested for one person.

## 6. The face book

The book keeps the user's answers, never the algorithm's suggestions. Resetting the index, turning face
search off and deleting its data leave it; only an explicit request to forget it clears it.

### 6.1 Named people

A person has a stable ID and a nonempty name, compared without regard to case. Renaming keeps their
confirmations and rejections; another person's name cannot be taken, and the Faces view offers to combine
the two instead ([§9.2](#92-faces)). Unknown is not a person.

Renaming a person whom pictures also name in their metadata asks whether to change the name there too:
**Change in files**, the default, writes the new name into those pictures ([§10](#10-saving-names-to-files));
**Only in Diffractor** changes the book alone; **Cancel** changes nothing. Either way the old name stays a
former name, so pictures still using it stay theirs and the person stays one row.

Combining two people keeps the target's name and ID, either name possibly known only from files; the
source's names refer to the target from then on, through later renames and combinations, and no file
changes. Confirmations and rejections move to the target; where that would both confirm and reject one face,
the confirmation wins. Other rejections and hidden flags stay. One Undo restores the people, names and marks,
including names known only from files, until the next book edit.

### 6.2 Marks

Each marked face records three independent facts:

- **Identity:** one confirmed person, or unknown. A suggestion confirms nothing.
- **Visibility:** hidden or visible, independent of identity.
- **Rejections:** every person ruled out for this face. **Not Zac** adds Zac and removes a confirmation of
  him, hiding nothing and rejecting no one else.

Assigning a face to Alex shows it, confirms Alex and removes only Alex from its rejections. One Undo restores
the identity, visibility and rejections of every face a placement touched and removes a person it created,
until the next book edit.

A mark on a measured face has a portable key: the model, every signed byte of the face hash, and the exact
bits of its positive, finite scale. Only an exact match recovers a decision - not a similar face, nor the same
bytes from another model - and copies of a face share one decision, with no picture needed to keep it. Local
marks also keep the picture's path, upright box and position: one with a key reads that key's current
decision, and one without, as an older book holds, finds its face by overlap
([§4.5](#45-named-regions-and-found-faces)). Once a scan identifies a signature, compatible local marks are
bound to it; conflicting ones stay local, since scan order must not decide whom to confirm, and binding never
replaces an existing exact decision.

Marks follow their pictures, so nobody sorts a face twice:

- **Renamed or moved by Diffractor** - Rename, the Rename view, Move to folder, cut and paste, Import moving
  its sources - a picture takes its marks, and a folder those of everything in it. A picture moved onto
  another's path takes its place, and the replaced picture's marks go.
- **Turned by its declared orientation alone**, its marks turn with it, as its record does
  ([§3.2](#32-the-scan)).
- **Changed in its pixels**, here or elsewhere, it is read again, and each mark goes to the face found again
  that resembles its face by 0.8 or more - far above any two people - the likeliest first, comparing only
  faces one model described. A mark whose face is not found so stays, on its own face or waiting while its
  face is lost, unless the edit moved its face away and another face is there now; then it goes, rather than
  mark someone else.

A picture renamed or moved by another program recovers its portable decisions once scanned; marks without a
signature wait under the old path.

### 6.3 Rebuilding the overview

The book stores no matches: each pass applies the current marks to the current scan and compares the
remaining faces with the named people. A legacy hidden flag applies only while the book has no mark for the
face.

### 6.4 The face database

`diffractor-people.db`, beside the index in the profile's data folder, is not a cache. It holds people
(`face_people`), former names and their owners (`face_aliases`), portable decisions and rejections by
signature, and local marks - folder, filename, upright box and position - linked to their signatures where
known; never comparisons, suggestions or detector indices. Path lookup distinguishes case on Linux and
matches ASCII case variants on Windows.

The database thread loads the book at start. Each edit writes what it touches in one transaction, in commit
order. A failed transaction rolls back, and the whole book - every person, former name, signature and mark -
is then written over the file every two seconds until a write succeeds, skipping edits a whole write already
holds; meanwhile the Faces view says the latest changes are not saved, and Export face data keeps a copy. The
schema is version 4 (`PRAGMA user_version`); a version 2 or 3 book gains the missing tables in one
transaction, keeping its IDs, names and marks. Only an empty file is initialized; any other schema is refused
and left untouched. A database that cannot be opened leaves the book in memory for the session, and an export
saves its portable evidence.

One window writes the book: the first to open the database claims it, by a lock named for its path. Any other
opens it read only - a version 4 book, never upgraded or created - refuses every change and says another
window is changing faces, offering no command, shortcut or button that would change the book. Every two
seconds a read-only window claims the database once the writer has closed, reading the book whole again
first, or else rereads it if the writer has changed it (`PRAGMA data_version`).

The writer keeps `secure_delete` on, so what an edit replaces and what Forget removes is overwritten rather
than left in free pages, and a forget that empties the book compacts the file (`VACUUM`); a read-only window
holding the file only postpones that to the next forget.

### 6.5 Saving and moving the face book

**Export face data** writes a version 4 UTF-8 CSV of people, their former names and `signature` records -
each a model ID and face hash with its decisions - as `to_csv` lays it out. No filename, folder, box or
position is exported. A path-only decision has no portable key: it is counted as left out and stays local
until scanning its picture makes it portable. The file holds face descriptors and names, so it is private.

**Import face data** reads versions 4, 3 and 2 (2 has no former names). People are joined by current or former
name, keeping the local ID and spelling and adding names not known here; an imported person whose names
belong to two local people refuses the import, unchanged - combine them first. Person IDs are remapped. A
version 4 signature can be restored before its picture is available, and matches once a scan finds it
anywhere. Import never reassigns a confirmation: a local rejection blocks an incoming confirmation, and a
local confirmation is never turned into a rejection; otherwise rejections are joined, and hiding in either
book is kept. Reimporting a file changes nothing. Older `face` records stay bound to their path and box until
a scan binds them. An unsupported version, a malformed record, a duplicate signature or a dangling person
reference refuses the import before anything changes.

## 7. Recognizing people

The overview combines one generation of the face store with the face book and the indexed metadata. It is
made off the UI thread whether or not the Faces view is open, on a thread of its own with its comparisons
shared out among free scan workers ([implementation](implementation.md#async-execution)), and gives the same
answer on any number of threads. Book edits, metadata updates and scan progress start a new pass, abandoning
one the book has moved on from. The result is published whole, and the overview never writes a name, a mark
or a file.

### 7.1 How a face is placed

**People** lists named people, including those known only from files or offered a face by a picture's names
([§7.2](#72-suggestions)). An explicit confirmation and a matched, uncontradicted named region are both
trusted, and a confirmed face stays confirmed however poor its resemblance. **Unknown** holds the visible
unassigned faces nobody is suggested for, unclear ones included; **Hidden** only faces explicitly hidden. No
person is inferred: Unknown's groups are only an order ([§9.1](#91-people)).

A mark that assigns someone else, rejects the region's person or hides the face overrides the region;
rejecting *another* person does not. An unmatched name still identifies the picture for search, but no face,
and trains nothing.

### 7.2 Suggestions

Each person has one recognition profile, from their visible confirmed and file-named faces with face hashes,
and from imported visible confirmations whose photos are absent. Only the same model and exact signature
restore a confirmation; anything else is at most suggested. A profile keeps individual appearances, not an
average - up to 64, spread far apart in appearance. Separately, up to three confirmed portraits represent a
person: a good face ([§4.4](#44-quality)) first, then different years and poses, photos before frames of
video; a missing date excludes none.

An unassigned face is offered only to its closest eligible person, resembling them by at least `confident`
and beating both the runner-up and that person's negative examples by `margin` ([§5.3](#53-comparing-faces)).
Within a picture the strongest clear offer goes first, and nobody confirmed or offered there gets another
offer there. The user or a file can name two faces of one picture alike; resemblance never overrules them.

A picture's names offer a face too: where a picture has one unplaced face and its metadata names one person
without placing them - in `Iptc4xmpExt:PersonInImage`, or a region without an area - that face is suggested
for them, and a person known only so appears under **People** with that offer. Nothing is offered by a
picture with two faces or two names, or with a face region, which places its own name, nor to a person
rejected for the face; a clear resemblance to someone else wins over the picture's name.

**Not Zac** rules Zac out for that face and nobody else, and its face hash becomes negative evidence: a
candidate must beat its likeness to his negative examples by the same margin. The exclusion itself holds even
without a hash. Hidden faces and suggestions never train a profile. The overview keeps each profile and each
face's exclusions, so naming a face ranks people by the same evidence ([§9.5](#95-naming-a-face)).

A person's confirmed or named faces and suggested faces are listed apart, strongest first, each scored against
*other* examples; a face with no hash, or no other usable example, has no score rather than a perfect one.
Scores only order faces and are never shown, since a resemblance is not a probability. A person is counted by
the pictures they are confirmed or named in, with suggestions beside it - **12 pictures · 3 suggested** - in
People, the sidebar and on a picture. An offer appears only on its person's page, returns to Unknown if it
loses its match, and shows a question mark on its picture. Nothing is hidden automatically.

## 8. Searching

| Term | Lists | Needs |
| --- | --- | --- |
| `person:"Jane Doe"`, or `people:` | Pictures naming her, or with a face confirmed or suggested as her | Nothing for names in metadata; face search for the face book's decisions and suggestions |
| `person:jane`, unquoted | Pictures naming anyone with a word of their name starting so: Jane Doe, Janet; `person:doe` every Doe | As `person:` |
| `person:zac person:jana` | Pictures naming both, as any two terms find what holds both | As `person:` |
| `zac`, `zac walker`, plain text | Beside plain text's other matches, pictures naming anyone with a word of their name starting with each word | As `person:` |
| `zac!`, `person:zac!` | Only pictures naming, or with a face confirmed as, someone with a word of their name starting with Zac; no suggestion, no ordinary text | As `person:` |
| `person:"Zac Walker"!` | The same confirmed-only search for the whole name | As `person:` |
| `with:person`, `without:person` | Pictures that name anyone, or no one, by metadata or a held face (not a suggestion) | As `person:` |
| `@face` | Pictures with at least one face found | Face search |
| `face:IMG_1234.jpg:2` | Pictures with a face resembling the second face found in IMG_1234.jpg | Face search |

A name is found by the start of a word - after a space or punctuation such as a hyphen or an apostrophe - so
`person:ann` finds Ann, Annabel and Mary-Ann but not Joanna; a wildcard, `person:Jane*`, is matched against
the whole name, and quotes ask for the whole name. The terms Diffractor writes for a person - a completion
chosen, the sidebar's faces, the Faces view's **Person search**, a name in an item's details - are quoted, so
each lists that person alone. A leading `!` still means *not*, and `"zac!"` is literal text. The indexed and
item-by-item searches both apply the face book's corrections before counting names from metadata.

Typing a name, or part of one, in the address bar offers a `person:` completion, names only the face book
holds included - those with a word starting with what is typed first, then those holding it inside a word -
each with how many pictures it lists, and keeping a typed `!`; choosing one writes nothing to files. A
representative face replaces the person icon once one is available.

A face term spells a face, not a person: a file and a position in its stable order ([§4.1](#41-detection)),
`:1` being the default and left out, so it outlives a new suggestion. It means the same face as long as the
picture's pixels and the model do - a face found in a named region follows the detector's
([§4.5](#45-named-regions-and-found-faces)) - and `@face:IMG_1234.jpg:2` is read as `face:IMG_1234.jpg:2`.
It names a file name, not a file, so that it can be read and typed: every file of that name answers it - at
most eight, in path order, so a common name cannot make one term cost the collection - and a name no file has
any more, or no face in that position, lists nothing rather than something else. A file name holding a space,
a bracket, a comma or a quote is written quoted, `face:"IMG_1234(1).jpg:2"`.

A listing these terms answer - plain text among them, which finds the face book's names - runs again as face
search publishes a new overview, once a minute while it scans and after each change the user makes, and when
Items is shown again after another view; people previews follow the same overview. Face terms answer in the
indexed collection only, names wherever metadata does, and `@face` is offered in completion only while it
finds something.

A video a search lists is marked where the faces it was found by are seen ([§3.3](#33-videos)): a bar along
the foot of its scrubber, in the orange of a search's matches, for each stretch. `@face` marks every face,
`face:` the faces at least `similar`, a person their faces as the search lists them - suggestions too unless
`!` - and `with:person` the faces held for anyone.

## 9. The Faces view

Faces opens from the navigation toolbar, the Navigate menu or the sidebar ([design](design.md#faces)), or from
a face on a picture. People are on the left and faces in the middle. The panel on the right says what the view
is for and that Diffractor learns as the user confirms faces ([§7.2](#72-suggestions)), then holds **Scan for
faces in photos** and **Scan for faces in videos** ([§3.1](#31-turning-it-on-and-off)) with the progress and
**Pause**; **Backup** - **Export face data** and **Import face data** ([§6.5](#65-saving-and-moving-the-face-book)),
a privacy note and where the data is kept; and **Delete face data**. The filter and the face-size slider lead
the status bar.

Review commands act on the visible selection and are shown only where they apply
([design](design.md#command-availability)), and none that would change the book is offered in a window that
only shows faces ([§6.4](#64-the-face-database)). **F2** names the selected faces or renames the selected
person, **Delete** rules out the person shown, **C** confirms the selected suggestions, and **H** hides the
selected faces or shows hidden ones again.

### 9.1 People

**Unknown** lists the faces still needing a person, in review order. Clear faces that resemble one another
form groups - each joined to the clear face most like it, where alike enough to offer - listed under **Similar
faces · N** headings, the largest first, so the people photographed most are named first; clicking a heading
selects its faces. Then come single clear faces under **Unique faces**, and last, however alike, those too
small or unclear to carry an identity ([§4.4](#44-quality)) under **Small or unclear faces**. The scroll bar
names each heading's band, as Items' does, wrapping names so the bar is no wider than the widest word.

Where the book can be changed, a group's heading ends with **Name all**, naming every face under it as one
person through the popup **F2** opens ([§9.5](#95-naming-a-face)), and **Hide all**; the unique and unclear
headings, of many people, have **Hide all** alone. Each acts as selecting the faces and pressing **F2** or **H**
does, leaving the same receipt and **Undo**, its bubble saying what it will do to how many faces.

Groups are only an order: they make no person, assign no name and remove no face. While Unknown stays selected
its order holds - as faces are named and their lookalikes become suggestions, groups keep their places and
headings, and newcomers follow what is shown in their group or section; choosing Unknown again, or returning
to Faces, sorts it afresh. Naming one face can take several from Unknown unconfirmed: they are under the
person's **Suggested** heading. **Hidden** lists the explicitly hidden faces.

Each person has one row, with the pictures they are confirmed in, the faces suggested for them and up to three
representative confirmed faces. People are filed by family name - the last word, before a suffix such as Jr.
or III, or the first word of a Chinese, Japanese or Korean name written with a space - or by the whole name
where it has none; two or more sharing one are listed under a heading for it, which folds the list only, and
a filter still finds them. The rail scrolls, resizes with the pane and marks selected rows, offscreen ones
included; a name too long ends in an ellipsis, shown whole on pointing.

### 9.2 Faces

A person's page lists **Confirmed** then **Suggested**, strongest first; **Confirm all**, ending the Suggested
heading, confirms every suggestion once the user agrees. A low score never revokes a name. Pointing at a
suggestion shows three shortcuts for that face alone: **Confirm** at the top left, **Not this person** at the
bottom left and **Name as someone else** at the bottom right ([§9.5](#95-naming-a-face)).

Dragging faces onto a name assigns them, and onto **Hidden** hides them. **Not Zac** excludes Zac without
hiding the face or stopping it being named Alex; **Hide** is visibility, not rejection; **Show again**, or
naming a hidden face, reveals it. Renaming someone to an existing name asks **Combine people?**
([§6.1](#61-named-people)): **OK** combines them under the destination's name, **Cancel** changes nothing. A
placement leaves a receipt in the status bar linking to its destination, with **Undo**, whose bubble says what
it reverses, until the next edit.

The page holds its place through confirmations, drags and new suggestions: a visible heading keeps its screen
position, and elsewhere a nearby face still in the same section stays put. Unknown and Hidden only lose faces
while shown, so there the first heading or face still listed keeps its place, or takes that of the first that
left, and the rest close up.

Pointing at a face shows its whole photo upright, within a third of the window's width and height, the face
outlined, with the name - and **Suggested** for an offer - above, and the filename, date and place below. A
video's face is shown in its frame, with its time beside the filename; until that frame is read, the video's
thumbnail stands in, unmarked. Double-clicking a face, or Enter, opens its picture in its folder, or its video
paused at the face's frame ([§9.3](#93-faces-on-a-picture)). **Find similar faces** searches for one face
without naming it.

### 9.3 Faces on a picture

On a picture shown whole and upright, pointing at a face shows its name - with a question mark if only
suggested - how many pictures the person is confirmed in and how many faces are suggested for them, and a
strip of buttons centred below the face, inside the view. The strip stays while the pointer moves onto it,
even as a scan or an overview reads the picture's faces again, and the bubble stands beside face and strip,
never over a button. The first button opens the face in Faces. Where the book can be changed, a suggestion
adds **Confirm**, **Not** and **Name as someone else**, and any other face a naming button, naming or
renaming it in the popup Faces uses ([§9.5](#95-naming-a-face)). Hidden faces are not shown, and these
actions change the face book, not the file.

**Ctrl+F2**, also on the picture's menu, names the face of the picture shown or selected where it has just
one, offering its suggestion first; otherwise it says why - no face found, more than one, face search off, or
the face book changed by another window.

A video shows its faces only while paused on a frame they were found in; playing hides them. One opened from
Faces is paused on the face's frame, the face outlined and its strip shown until the pointer comes onto the
video. Ctrl+F2 names a video's face where the video holds one person's face, whatever frame is shown.

### 9.4 The sidebar's faces

The sidebar's Faces button opens the view even before previews are ready. Its portraits are of named people,
those confirmed in the most pictures first, each saying how many pictures and suggested faces the person has
and opening that person's search.

### 9.5 Naming a face

**F2**, **Name all**, **Name as someone else**, a picture's naming button and **Ctrl+F2** ask whom the faces
are in one popup, laid out as Copy to folder is: the faces being named beside the title - up to seven, then
how many more - and a search box, which has the keyboard from the start, over a list of people, each with a
portrait and how many pictures they are in. Before anything is typed it lists the people the faces most
likely are, then the names given faces this session (kept nowhere else), then the people in the most
pictures; typing narrows it to the names that match, in that order, adding **New person** where nobody is
called what was typed. Up and Down choose a row; Enter or a double-click names the faces as it and closes the
popup, and a name typed with no row chosen is used as typed. The likeliest person starts chosen, so Enter
alone names the faces as them - unless every face is named already, a rename, which starts with nobody
chosen. Naming a picture's only face offers **Go to the next picture afterwards**, which is remembered.

The likely people are ranked by the profiles suggestions use ([§7.2](#72-suggestions)) - each by their average
likeness to the faces, a large selection judged by 64 spread through it - and none below `similar`
([§5.3](#53-comparing-faces)): the most alike, and up to two more within `margin` of them, whom Diffractor
cannot tell apart, as so often young siblings, and so never chooses between. Nobody is offered whom a face is
ruled out for, who is confirmed on another face of its picture, or whom it resembles no more than the faces
ruled out for them; nor anyone the faces are already shown as, except by **Ctrl+F2**, which offers the
suggestion first, so Ctrl+F2 then Enter confirms it.

## 10. Saving names to files

**Save names to files** is not offered in this release: `face_save_names` is never enabled, so no button,
key, menu or agent command reaches it. Its plan is kept for when it is: each visible confirmed face named in
its picture, a region's name removed only where that person was explicitly rejected for that face, and the
plan reviewed before the metadata batch writes it ([file I/O](file-io.md#failure-and-recovery)).

Renaming a person files name - one the face book holds ([§6.1](#61-named-people)) or one known only from
files - does write files, as every name write does: into the picture's XMP, in the file or its sidecar
([file I/O](file-io.md#sidecars)), read back as every write is ([file I/O](file-io.md#coherent-read-back)),
as an `mwg-rs` region of type `Face` with the name, its area the face's box in the stored picture's space
([§2.2](#22-where-a-region-is)), and `mwg-rs:AppliedToDimensions` the stored picture's pixels.

- Everything else in the packet stays - other faces, unnamed regions, pets and focus areas - and a face
  already in a region is named there. One `mwg-rs:AppliedToDimensions` covers every region, so each region
  kept is written in the stored picture's space too.
- A packet whose areas no longer fit its picture ([§2.2](#22-where-a-region-is)) refuses a name that would
  place a face in it, saying why; a rename or a removal places nothing and still reaches it.
- Names only in Microsoft's regions are copied into `mwg-rs` when a face is first named there. A rename or a
  removal changes the name wherever the packet states it - `mwg-rs`, Microsoft's regions and
  `Iptc4xmpExt:PersonInImage` - as naming a face someone else was named for does.
- Names are never written as keywords, which are the user's own vocabulary, and nothing else face search or
  the face book holds - face hashes, visibility, rejections - is ever written to a file.

A name write is metadata-only and Diffractor's own, so the file's faces stay current ([§3.2](#32-the-scan)).
Regions do not move with the pixels: Rotate, or Edit's turn, straightening, perspective and crop, leave them
measured on the old picture, so `mwg-rs:AppliedToDimensions` shows them stale - names kept, areas dropped
([§2.2](#22-where-a-region-is)). A turn written as the declared orientation alone moves nothing.

## 11. Privacy and recovery

- Detection, embedding, suggestions and search run on this machine. Face search uses no network and writes
  nothing about a face to the log or to feature-use counts. A crash report, sent only when the user allows,
  carries a minidump ([crash investigation](crash.md#1-what-a-report-contains)) that can hold memory face work
  was using; nothing else takes face data off the machine.
- Everything face search finds is derived, returns by scanning again, and goes with Delete face data or an
  index reset ([§3.1](#31-turning-it-on-and-off)). The face book goes only when the user asks to forget it.
  What Delete face data and Forget remove is overwritten rather than left in the databases' free space.
- Names are the user's metadata, kept and recovered as all metadata is
  ([file I/O](file-io.md#failure-and-recovery)). An export holds named biometric signatures and has no paths,
  but identifies people all the same, so it is kept private ([§6.5](#65-saving-and-moving-the-face-book)).

## 12. Limits

These are decisions rather than omissions:

- A video is read at its key frames, at most 256: a face shown only between them is not found, and scrubber
  marks are only as exact as the key frames - about a second in a phone's video, several in a film. Each
  person is kept once a video, by their clearest face, which alone suggests or finds the video.
- Diffractor names nobody: every name was given by a user, and everything Diffractor offers - a resemblance,
  or a name a picture states without saying which face - is a suggestion until confirmed.
- No face hash is written to a picture, and exported signatures serve only the model that made them
  ([§6.5](#65-saving-and-moving-the-face-book)).
- Young children are told apart only by comparison ([§5.3](#53-comparing-faces)): a face as like two children
  as each other stays unknown rather than guessed, and confirming examples from different ages helps without
  making a second identity.
- A rejection is specific to a person and a face; to correct one made in error, name that face as the person.
- Faces in paintings, sculpture and drawings are found like any other, but the network was made for
  photographs and they resemble one another more than people do, so they gather in Unknown's groups and are
  offered more often; a group's **Hide all** hides them.

## Where this lives

| Concern | Source |
| --- | --- |
| Names from files: regions read, packed as stated, and turned upright as they are unpacked | libfiles' [properties.h](https://github.com/ZacWalk/libfiles/blob/main/include/libfiles/properties.h); `face_regions_of` in [files.cpp](../src/files.cpp); `pack_regions`, `unpack_regions` and `upright_regions` in [model_faces.h](../src/model_faces.h) |
| Person terms, their completion and the sidebar's faces | [model_search.cpp](../src/model_search.cpp), [model_search_index.cpp](../src/model_search_index.cpp), [model_property.cpp](../src/model_property.cpp), [model_index_summary.cpp](../src/model_index_summary.cpp), [app_sidebar.h](../src/app_sidebar.h) |
| Turning face search on and off, and deleting its data | `turn_on_face_search`, `turn_off_face_search` and `delete_face_data` in [view_faces_actions.cpp](../src/view_faces_actions.cpp); `face_search::turn_on`, `turn_off` and `delete_data` in [model_faces_search.cpp](../src/model_faces_search.cpp); the Faces card in [view_options.cpp](../src/view_options.cpp) |
| Detection, alignment, embedding, quality, face hashes and the face record, and the tuning | [model_faces.h](../src/model_faces.h), [model_faces.cpp](../src/model_faces.cpp); libmug's [detect.h](https://github.com/ZacWalk/libmug/blob/main/include/libmug/detect.h) and [embed.h](https://github.com/ZacWalk/libmug/blob/main/include/libmug/embed.h); [DiffractorLibmug.cmake](../cmake/DiffractorLibmug.cmake) |
| The decode a picture's faces are read from | `decode_for_faces` in [model_faces_search.cpp](../src/model_faces_search.cpp); `still_decode_request` in [files.h](../src/files.h) |
| A video's faces: key frames read and decoded alone, and made one record | `read_key_frame`, `key_frame_codec` and `av_decode_key_frame` in [av_format.cpp](../src/av_format.cpp); `read_video`, `read_video_frame`, `finish_video` and `read_face_frames` in [model_faces_search.cpp](../src/model_faces_search.cpp); `read_video_slices` in [model_faces.h](../src/model_faces.h); `merge_video_frames` in [model_faces.cpp](../src/model_faces.cpp) |
| The face store, the scan and its admission gate, face terms and the faces on a picture | `face_store` and `scan_io_gate` in [model_faces_search.h](../src/model_faces_search.h); [model_faces_search.cpp](../src/model_faces_search.cpp) |
| The face book: people, face evidence and the CSV file | [model_faces_book.h](../src/model_faces_book.h), [model_faces_book.cpp](../src/model_faces_book.cpp), with `to_csv` and `import_csv` |
| Marks following their pictures: renamed and moved, turned, and changed in their pixels | `paths_moved` and `restamp_face_record` in [model_index.cpp](../src/model_index.cpp), told of moves by [model_items.cpp](../src/model_items.cpp), [view_rename.cpp](../src/view_rename.cpp), [app_commands.cpp](../src/app_commands.cpp) and [app_util.cpp](../src/app_util.cpp); `moved`, `turned` and `follow` in [model_faces_book.cpp](../src/model_faces_book.cpp), followed from `apply_results` in [model_faces_search.cpp](../src/model_faces_search.cpp) |
| The face database: one writer, read-only windows, retries and secure deletion | [model_db_faces.h](../src/model_db_faces.h), [model_db_faces.cpp](../src/model_db_faces.cpp); `database::open_faces`, `write_faces` and `poll_faces` in [model_db.cpp](../src/model_db.cpp) |
| The overview: placement, suggestions and a picture's names' offers, Unknown's order and portraits | [model_faces_groups.cpp](../src/model_faces_groups.cpp); `queue_overview` and `lend_faces_work` in [model_faces_search.cpp](../src/model_faces_search.cpp); `dot_values` in [model_faces.cpp](../src/model_faces.cpp) |
| The face record in the index: loaded, written, deleted and stamped afresh | `prop::cache::faces` in [model_property.h](../src/model_property.h); `load_face_records` and `delete_face_records` in [model_db.cpp](../src/model_db.cpp); `apply_scan_now` and `spend_face_restamps` in [model_index.cpp](../src/model_index.cpp) |
| The Faces view: its panes, drags, crops, commands and panel; saving names, export and import | [view_faces.h](../src/view_faces.h), [view_faces.cpp](../src/view_faces.cpp), [view_faces_actions.cpp](../src/view_faces_actions.cpp); [app_commands.cpp](../src/app_commands.cpp), [app_toolbar.cpp](../src/app_toolbar.cpp) |
| Faces on a picture | `picture_faces`, shared by `photo_control` and `video_control`, in [ui_controls.h](../src/ui_controls.h) and [ui_elements.cpp](../src/ui_elements.cpp) |
| A video opened at a face, and its scrubber's marks | `faces_view::double_click_cell` in [view_faces.cpp](../src/view_faces.cpp); `_pending_media_start` in [model.h](../src/model.h), taken by `load_display_state` in [model.cpp](../src/model.cpp); `face_spans_found` in [model_search.cpp](../src/model_search.cpp); `scrubber_element` in [ui_controls.h](../src/ui_controls.h), given its marks by `make_media_transport` in [ui_music.cpp](../src/ui_music.cpp) |
| Naming a face: the people offered, the popup and Ctrl+F2 | `likely_people` in [model_faces_groups.cpp](../src/model_faces_groups.cpp); `person_picker`, `ask_face_name`, `name_picture_face`, `only_face_on_picture` and `faces_view::name_cells` in [view_faces_actions.cpp](../src/view_faces_actions.cpp); `face_name_picture` in [app_commands.cpp](../src/app_commands.cpp) and [app_toolbar.cpp](../src/app_toolbar.cpp) |
| Writing names: the edits and the regions written | `face_name_edits` in [files.h](../src/files.h); `apply_face_names` in [metadata.cpp](../src/metadata.cpp) |
| The agent tools over faces | [app_mcp_faces.cpp](../src/app_mcp_faces.cpp); [agents](agents.md#5-the-tools) |

Tests ([testing](testing.md#taxonomy)): [test_faces.cpp](../src/test_faces.cpp) covers most of the above;
[test_av.cpp](../src/test_av.cpp) reading and decoding key frames alone; [test_app.cpp](../src/test_app.cpp) the
face commands and marks moving with their pictures; [test_index.cpp](../src/test_index.cpp) face records across
metadata writes and search parity; libmug's own tests its networks; and the isolated
[test_mcp_session.py](../tools/test_mcp_session.py) profile naming, confirming, rejecting, hiding, undo, export
and import in the running application.
