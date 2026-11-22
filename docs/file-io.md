# File I/O

This document owns the path from media bytes to displayed pixels, media and sidecar writes, coherent
read-back, and failure outcomes. [Metadata](metadata.md) owns property-to-tag mappings;
[Rendering](rendering.md) owns image budgets and presentation; [Zoom](zoom.md) owns scale and center;
and [Design](design.md) owns user-facing operation semantics.

## Contract

- File access, decoding and writing run off the UI thread and outside index locks.
- Display uses the best available representation at once and replaces it only with a better one.
- Metadata-only changes avoid pixel and thumbnail reloads when presentation is unchanged.
- A staged write leaves the live destination untouched until replacement, and makes a requested
  backup before replacement begins.
- A caller that reviewed a free destination can make staged publication fail if the destination
  appears before the final move; one that reviewed a replacement can require the destination still to
  match that review just before the swap (`file_update_publish`).
- An in-place metadata write trades full rollback for bounded cost: failure is reported and the file
  rescanned, without the staged path's prior-byte guarantee.
- A successful write publishes the bytes and modified time actually written, including on SMB.
- A writability probe exclusively claims its own temporary file; an existing file at a probe name is a
  collision, never permission to truncate or remove it.

## Reading: The Display Phase Ladder

Selection display starts from the browser thumbnail when available, and works up through the
representations that apply without blocking input:

| Phase | Representation | Applies to |
| --- | --- | --- |
| 0 | Correctly shaped neutral placeholder from indexed dimensions | Every item without pixels yet |
| 1 | Decoded item thumbnail | Every displayable item |
| 2 | Embedded preview | RAW files |
| 3 | Source image decoded for the display | Non-RAW images |
| 4 | Full RAW development | RAW files on request |

Phases that do not apply are skipped. Publication checks item identity and generation, so stale work
cannot replace a newer item or a better phase. Resolution upgrades dissolve within one item when
animations are enabled; navigation to another item is immediate.

Decode requests use display size where the codec supports scaled decode; what stays decoded, and
texture upload, are [Rendering](rendering.md#image-budgets)'s. A bitmap thumbnail scan obeys the
encoded-byte ceiling whole-file loads do: a file over it may still contribute header and metadata, but
is not read into memory or published as a partial thumbnail.

Archive listings keep usable entries after recoverable header warnings and bound the headers examined,
entries whose names cannot be read included. Archive timestamps are converted from Unix time into the
application's UTC units. ZIP export declares UTF-8 names and keeps classic headers, switching an entry
to ZIP64 only when its size or offset does not fit 32 bits.

### When An Image Cannot Be Shown

Header dimensions, estimated decoded bytes, codec limits and available memory are checked before a
large allocation. A refused image reports its source dimensions and a bounded reason rather than
keeping a thumbnail that could be mistaken for the full image. Decode errors - codec initialization and
scratch allocation failures included - are isolated to the item; a malformed file does not retire the
worker. Compressed metadata is accepted only once its compressed stream completes.

## Writing

`files::update`, and `files::update_av` for Edit's lossless video and sound edits, run on
`async_queue::work` and share one path:

```text
no effective change                      -> return without touching disk
metadata-only change that fits the file  -> patch the destination in place
metadata-only format using a sidecar     -> stage and replace the sidecar
all other changes                        -> stage, edit, back up if requested, replace, rescan
```

The stage is created in the destination folder. A metadata edit starts from the source's packet, keeping
what the edit does not mention. For a format libfiles writes, a staged metadata-only edit - one that does
not fit the file, a Save As, or one with a backup - is planned against the source before anything is
staged: one that fits the space the file holds patches a copy, which the file system may make without
moving the bytes, and any other is written to the stage in one pass over the source rather than copied
and rewritten. Other formats are copied, then updated. When a sidecar must change too, a rollback copy
protects the media and sidecar pair across the two replacements. Temporary paths are removed on every
bounded failure path.

A requested `.original` backup is made before replacement and never overwrites an existing one.
Metadata commands do not imply a backup; callers ask for one.

### JPEG XL, HEIC And AVIF

libfiles updates the metadata of a JPEG XL, a HEIC and an AVIF in the file's own boxes or items,
compressed boxes included, as its [How it writes](https://github.com/ZacWalk/libfiles/blob/main/docs/design.md#how-it-writes)
says: a HEIF without a packet gains the item libheif would add, EXIF the file holds takes the edit's EXIF
fields, and a JPEG XL made from a JPEG keeps the boxes that JPEG is given back from at their size.
Rotate writes a quarter turn as the file's own orientation - a JPEG XL's codestream header, a HEIF's
image rotation and mirror and its thumbnail's - with the EXIF made to agree, rather than encoding the
picture again; a JPEG XL whose header cannot take the turn is turned as a picture instead. Diffractor
encodes AVIF, through libfiles' libheif and libaom, but not HEIC, whose encoder for libheif, x265, is
GPL: any other edit encodes an AVIF's or a JPEG XL's picture again, and saves a HEIC's as a copy of its
own - a JPEG, or an AVIF where the HEIC holds more than eight bits, HDR or a gain map
([design](design.md#metadata-editing-and-files)).

A pixel write producing a JPEG XL or an AVIF - an edit, Convert, Save As, a shared copy - encodes the
packet an update would write (the edit, and from another format what the source shows that its packet
lacks) into the new file's own boxes or items beside the source's EXIF, so no update follows. The file's
own orientation holds the turn it ends with - a HEIF reader applies it whatever the EXIF says - and the
EXIF is made to agree. A HEIF is decoded turned by its item's rotation and mirror, which the EXIF
orientation a phone writes beside them describes again, so a copy made from one is written upright with
that orientation reset. A JPEG XL moved unchanged is copied as it is.

A JPEG written as a lossless JPEG XL, at its own size and turned by quarter turns at most, is repacked
rather than encoded from its pixels: libjxl recompresses its coefficients, commonly a fifth smaller,
keeping the JPEG to give back byte for byte, and the packet the JPEG XL is to hold - with what the JPEG
shows from its IPTC - is written into the JPEG first, so the JPEG given back holds it too. Such a JPEG XL
gives its JPEG back to a write that starts from its picture: saved as a JPEG at its own size it is that
JPEG, whatever quality a conversion names; saved as a JPEG XL it stays repacked, unless a conversion asks
for its settings or a lossless JPEG XL is asked for. Rotate turns it by its codestream's orientation where
its JPEG's EXIF holds an orientation to turn with it. Otherwise a turn, or any other edit, is made to the
JPEG as to a JPEG of its own - losslessly where a turn's blocks allow, else encoded again against its own
tables and segments - and the result repacked. libjxl cannot keep a CMYK, arithmetic-coded or 12-bit
JPEG, which is encoded from its pixels.

### Pixel Writes

A pixel write in the file's own format keeps how the file was compressed: a JPEG is encoded against its
own quantization tables; a WebP stays lossless if it was, or is written at no less than the quality
libwebp estimates it had; a JPEG XL stays lossless if it was, as does an AVIF coded as RGB itself, as a
lossless one is. Convert asks for its settings, so what it writes follows them. An animation - a WebP, a
PNG or a JPEG XL with frames, or a HEIF image sequence - shows one still: its first frame, or the image a
PNG or a HEIF keeps for a viewer that does not animate. Its picture is never written back over it, which
would keep that still alone: turning or editing one in place fails and leaves the file as it was, while a
copy made by Convert or Save As is made of that still.

A pixel edit loads its source as `decode_intent::edit` asks - at sixteen bits a channel where the file
holds more than eight: a 16-bit PNG, a HEIF or AVIF of ten or twelve bits, a JPEG XL of more than eight,
or a developed raw file - and works at that precision: PNG and JPEG XL are saved at sixteen bits, AVIF at
ten, or twelve for a twelve-bit source, and JPEG and WebP are rounded to a byte once, as they are
encoded. Where the decode budget holds only a byte a channel the edit is made at that, and the Edit
panel says so.

An HDR picture - PQ or HLG - is loaded as its signal, with the description libfiles reads of its colour
and light, which the AVIF, JPEG XL or PNG it is saved as writes back. Its geometry is edited as the
signal is, and its colour as the SDR rendition the Edit view shows: libfiles' `carry_sdr_edit` scales
the signal's light by what the adjustment did to the rendition. Saved as a JPEG it is that rendition with
a gain map made for it; as a WebP, the rendition alone. One too large to edit in HDR is edited as its
rendition, which never replaces it: Edit saves a change to its pixels as a copy, and a Rotate that would
have to re-encode it fails and leaves the file as it was - a quarter turn of a HEIF or a JPEG XL, written
as an orientation, still saves over it.

A picture whose ICC profile describes colours other than sRGB's - a phone's Display P3, a camera's Adobe
RGB (1998) - is loaded in those colours for an edit, which knows the profile (`picture_signal::profile`),
and its colour is edited as the sRGB rendition the Edit view shows: libfiles' `carry_srgb_edit` carries
what the adjustment did to the rendition back into the profile's colours, a pixel it left coming back
exactly as it was, and the profile is written with them, so a colour outside sRGB keeps what of its
gamut the edit does not take.

A gain map - Ultra HDR's, Adobe's, Apple's or ISO 21496-1's, in a JPEG, HEIF or AVIF - says how much
brighter each part of the photo is shown on an HDR display, and goes with the photo: turned,
straightened, corrected for perspective, cropped and scaled as the photo is, at its own resolution - in
a JPEG losslessly, with a lossless turn, where its blocks allow - and keeping its own values, which scale
whatever colour the edit gave the photo. A JPEG's is attached after the new photo with the
multi-picture index and XMP signalling its metadata calls for; an AVIF's is written as an ISO 21496-1
tone map, so a HEIC copied as an AVIF or a JPEG keeps it. PNG, JPEG XL and WebP hold none and leave it
out. A gain map that cannot be edited is left behind, and the photo then says none follows; the
metadata update after a re-encode keeps the index and the XMP's account of the gain map true to the
file. A copy in a format that cannot hold the photo's HDR or its gain map says so before it is written,
and Convert's formats say what each keeps.

Encoded JPEG, PNG, WebP, JPEG XL and AVIF buffers pass from libfiles into the displayed image's blob
without a second encoded-buffer copy.

### Pasted, Dropped And Copied Pictures

Pasted and dropped pictures are saved alike, as PNG under an auto-numbered name: a PNG the clipboard or
the drop offers - as browsers, Office and the Snipping Tool do - is written as it came, alpha included,
and otherwise the bitmap is encoded. Each is written to an owned temporary file - a paste's off the UI
thread - and published to the destination only once the stream and encoder commit. The captured
clipboard medium stays alive while encoding and is released on the apartment that obtained it, even
when the worker fails or is cancelled. A copied picture is offered as a bitmap, a device-independent
bitmap and a PNG, each the sRGB rendition Diffractor shows; the PNG keeps its alpha.

### Lossless Video And Sound Edits

`files::update_av` stages Edit's video and sound edits as an update does: `av_remux` copies the source's
packets into the stage with FFmpeg's muxer for the destination's format - the file's own, or MP4 -
trimmed, without the sound, or turned by the track's matrix, re-encoding nothing; an MP4 made from
another format starts where the source's timeline does. FFmpeg writes back only the tags its demuxer
read, so the stage is then given the source's XMP packet and the rating and tags the item shows, where
the copy's format keeps a packet; a format whose metadata is in a sidecar keeps that sidecar, and Save
As writes one beside a copy that cannot keep a packet. An MP4-family copy asked for its index first, or
whose source had it there, is rewritten last by `av_move_index_first`, its movie box moved before its
media and every chunk offset with it - last, because a metadata update writes a movie box that has to
grow whole at the end of the file. The stage replaces the destination, behind a backup if requested,
and is rescanned through its handle, from which the player reopens.

A source that cannot be read to its end fails the copy rather than saving the part before it - unless
the copy is repairing: then it reads on past what cannot be read, leaves out packets the muxer refuses,
reports what it left out, and replaces its own source only behind a backup. A save can be cancelled
until the stage would replace the destination, so one cancelled while its tags are written still stops.
A cancelled or failed copy removes the stage and leaves the destination as it was.

## In-Place Metadata

In-place writing is allowed only with no path or pixel change and no backup request, for every format
libfiles writes, which carries the in-place trait: JPEG, PNG, GIF, WebP, TIFF and DNG - one with no
sidecar - JPEG XL, HEIC and AVIF, PSD, the MP4 and ASF families, WAV, AVI, MP3, MPA and FLV. libfiles
plans the edit against the live file through one handle no one else may open meanwhile. An edit that
fits the space the file holds - a packet that keeps its length, EXIF values of the same size, ID3
padding, the JUNK after a RIFF chunk, or what is new appended at the end, as a TIFF's directories and an
MP4's boxes are - is patched where the file lies: nothing the file holds moves, the writes go in flushed
steps ordered so that an interruption leaves a file that reads whole with each field old or new, and a
rewrite that keeps its length writes only the span it changes. The patched file keeps its identity,
named streams and attributes. An edit that does not fit is staged, as above.

An in-place write that fails without changing the file - as one that loses a sharing race does - waits
for the file to unlock and is tried once more; one that changed the file fails at once. With no complete
prior copy, byte-for-byte rollback cannot be promised once the live file has changed.

## Sidecars

A format without embedded XMP keeps its metadata in a sidecar: the one beside the file, as Adobe's
`<name>.xmp` or darktable's `<name>.<ext>.xmp`, else a new `<name>.xmp`. A DNG holds a packet of its own,
but one with a sidecar beside it - under either name - is read from that sidecar and so written there
too, Camera Raw's rule for a DNG unless told to ignore sidecars; a DNG without one is written itself, as
Lightroom writes one, and only a sidecar known not to exist sends an edit into the DNG. A metadata-only
edit to a raw's sidecar changes only the staged sidecar; the media bytes stay untouched. A raw's first
sidecar starts from the effective source packet, so embedded XMP is not shadowed by an otherwise default
sidecar. An existing sidecar, even an explicitly empty one, stays the higher-priority source; an
unreadable one fails the edit rather than being replaced. For a combined media and sidecar update, the
media replacement completes first and is rolled back if the sidecar replacement fails, where the
platform supports that recovery.

Import treats automatic sidecars as companions, not extra rows. When collision resolution sends two
primaries sharing one sidecar to different basenames or folders, each destination gets the sidecar. A
completed companion copy is reused only by the same source and destination pair, and is revalidated
first. Move keeps the source sidecar until every dependent primary has moved; skipped, failed and
cancelled sources keep their metadata.

## Coherent Read-Back

Every successful write is rescanned so the index, thumbnail and displayed properties describe the new
bytes. A staged replacement keeps a handle to the staged file through its rename and rescans through
that handle, so a stale by-name SMB cache cannot return the old destination after the swap. Where a
filesystem cannot provide that, the fallback replaces by name and the result is marked accordingly.

Self-authored metadata-only changes publish the new metadata and modified time while keeping valid
pixels; pixel or presentation changes invalidate the display ladder. External changes are discovered by
folder comparison and treated conservatively, because their effect is unknown.

## Failure And Recovery

| Failure | Outcome |
| --- | --- |
| Stage or staged metadata cannot be produced | Live destination is unchanged; temporary files are removed |
| Requested backup cannot be created | Operation fails before replacement |
| Media replacement fails | Destination remains unchanged |
| Sidecar replacement fails | Media is restored from its rollback copy, or removed when it is new |
| In-place patch fails | One retry while the file is untouched, then a reported failure and rescan; no full rollback promise |

Cancellation stops future items in a batch and reports completed, failed, skipped and cancelled items
separately. It does not claim to undo completed writes.

Rename recovers a displaced, unfinished media file and all its sidecars as one group, under their
original names if available, else their reviewed names, else one collectively free numbered basename.
Recovery never places a sidecar beside an unrelated primary; a failed recovery reports the surviving
paths.

## Where this lives

- [files.cpp](../src/files.cpp): `files::load`; `files::update`, `files::update_av` and the
  `update_impl` they share - staging, sidecars, backups and write result publication; archive listing;
  `files::with_edited_gain_map`, which edits a JPEG's gain map as its photo was and attaches it; and
  `source_gain_map` and `gain_map_for`, the gain map a re-encoded photo is written with - its own,
  edited, or one made for an HDR picture a JPEG holds.
- [files.cpp](../src/files.cpp)'s `s_still_formats`: the still image formats recognised by signature,
  one entry each for detection, header scanning, loading and decoding.
- [metadata.cpp](../src/metadata.cpp): `metadata_xmp`'s `patch`, `stage_update` and `update` - the
  in-place patch, a staged metadata-only edit, and packet and sidecar updates.
- [media_edit.cpp](../src/media_edit.cpp): `apply_image_edits`, which edits an HDR picture's colour as its
  SDR rendition is shown edited, and a profile's colours as their sRGB rendition is.
- [view_edit.cpp](../src/view_edit.cpp): `edit_offers_avif`, `edit_save_loss` and `edit_photo_note`, what
  Edit offers and says of a photo's depth and range.
- [av_format.cpp](../src/av_format.cpp): `av_remux`, the lossless copy of a video or a sound file;
  `av_plan_edits`, what that copy can do and leaves out; and `av_move_index_first`.
- [util_zip.cpp](../src/util_zip.cpp): ZIP creation, entry names, per-entry ZIP64 headers, and entry
  timestamps.
- [model.cpp](../src/model.cpp), [model.h](../src/model.h) and [model_display.h](../src/model_display.h):
  `texture_state` and `display_state_t`.
- [model_index.cpp](../src/model_index.cpp): change discovery, scanning, and thumbnail scheduling.
- libdesktop's [files.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/platform/win/files.cpp):
  replacement, coherent file handles, backups, and the pasted picture's encoding and staged
  publication.
- [view_rename.cpp](../src/view_rename.cpp): displaced rename groups and cancellation recovery.
- [app_util.cpp](../src/app_util.cpp): Import planning, shared companions, and their rollback.