# File I/O

This document owns the path from media bytes to displayed pixels, media and sidecar writes, coherent
read-back, and failure outcomes. [Metadata](metadata.md) owns property-to-tag mappings;
[Rendering](rendering.md) owns image budgets and presentation; [Zoom](zoom.md) owns scale and center;
and [Design](design.md) owns user-facing operation semantics.

## Contract

- File access, decoding, and writing run off the UI thread and outside index locks.
- Display uses the best available representation immediately and replaces it only with a better one.
- Metadata-only changes avoid pixel and thumbnail reloads when presentation is unchanged.
- Staged writes leave the live destination untouched until replacement and preserve a requested
  original before replacement begins.
- Callers that reviewed a free destination can require staged publication to fail if the destination
  appears before the final move; callers that reviewed a replacement can require the destination to
  still match that review immediately before the swap.
- Approved in-place metadata writes trade full rollback for bounded cost; failure is reported and
  the result is rescanned. They do not carry the staged path's prior-byte guarantee.
- A successful write publishes the bytes and modified time actually written, including on SMB.
- A writability probe exclusively claims its own temporary file; an existing file at a probe name
  is a collision, never permission to truncate or remove it.

## Reading: The Display Phase Ladder

Selection display starts from the browser thumbnail when available. Work proceeds through the
applicable representations without blocking input:

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

Decode requests use display size where the codec supports scaled decode. Encoded data and browser
thumbnails are budgeted; decoded surfaces are retained only while visible or when no cheaper form can
recreate them. GPU textures are created lazily by the UI thread's render-owned draw context and may be discarded on
device loss without reopening the source.
Bitmap thumbnail scans obey the same encoded-byte ceiling as whole-file loads: a file over that
ceiling may still contribute header and metadata, but it is not read into memory or published as a
partial thumbnail.

Archive listings retain usable entries after recoverable header warnings and bound the number of
headers examined, including entries whose names cannot be read. Known archive timestamps are converted
from Unix time into the application's UTC storage units. ZIP export declares UTF-8 names and keeps
classic headers, switching an entry to ZIP64 only when its size or archive offset does not fit 32 bits.

### When An Image Cannot Be Shown

Header dimensions, estimated decoded bytes, codec limits, and available memory are checked before a
large allocation. A refused image reports its source dimensions and a bounded failure reason rather
than retaining a thumbnail that could be mistaken for the full image. Decode errors are isolated to
the item; a malformed file does not retire the worker.
Codec initialization and scratch-buffer allocation failures follow the same item-level failure
contract. Compressed metadata is accepted only after its compressed stream completes successfully.

## Writing

`files::update` is the single media-update entry point and runs on `async_queue::work`:

```text
no effective change                      -> return without touching disk
metadata-only change that fits the file  -> patch the destination in place
metadata-only format using a sidecar     -> stage and replace the sidecar
all other changes                        -> stage, edit, back up if requested, replace, rescan
```

The staged file is created in the destination folder. A metadata edit starts from the source packet,
preserving properties the edit did not mention. For a format libfiles writes, a staged metadata-only
edit - one that does not fit the file, or a Save As or backup - is planned against the source before
anything is staged: one that fits the space the file holds patches a copy, which the file system may
make without moving the bytes, and any other is written to the stage in one pass over the source
rather than copied and then rewritten. Other formats are copied, then updated. If a sidecar must also change, a rollback copy protects the media/sidecar pair across the two
replacements. Temporary paths are cleaned on every bounded failure path.

A JPEG XL's metadata is updated by libfiles like any other format's, its compressed boxes read with
libfiles' bounded Brotli decoder; one made from a JPEG keeps the boxes it gives that JPEG back from at their size.
Rotate turns one by the orientation its codestream's header keeps, as a JPEG's EXIF keeps one, without
encoding its picture again. Any other edit encodes its picture again. A pixel write that produces one - an
edit, Convert, Save As, a shared copy - reads the packet as an update would, applies the edit and, from another
format, what the source shows that its packet lacks, and encodes it into the file's own boxes beside the source's
EXIF; no update follows. The codestream holds the orientation the file ends with, which the EXIF is made to agree
with. A JPEG XL moved unchanged is copied as it is.

A JPEG written as a lossless JPEG XL, at its own size and turned by quarter turns at most, is repacked rather
than encoded from its pixels: libjxl compresses its coefficients again, commonly a fifth smaller, and keeps the
JPEG to give back byte for byte. The packet the JPEG XL is to hold - what the JPEG shows from its IPTC among it -
is written into the JPEG first, so the JPEG given back holds it too. A JPEG XL repacked from a JPEG gives that
JPEG back for a write that starts from its picture: written as a JPEG at its own size, it is that JPEG, whatever
quality a conversion names, with the metadata update that follows any write; and written as a JPEG XL again it
stays one that gives a JPEG back, unless a conversion asks for its settings or a lossless JPEG XL is asked for.
Rotate turns it by its codestream's orientation only where the EXIF its JPEG is given back with holds one to turn
with it; otherwise the JPEG is turned without loss where its blocks allow, or encoded again against its own
tables, as a JPEG's own is, and repacked. Any other edit encodes the JPEG again against its tables, with its own
segments, and repacks it. libjxl cannot keep a CMYK, arithmetic-coded or 12-bit JPEG, which is encoded from its
pixels.

A HEIC's or an AVIF's metadata is updated by libfiles in the file's own items: one without a packet gains
the packet item libheif would add, and the EXIF item it holds takes the edit's EXIF fields. Rotate turns one
by its image's own rotation and mirror - and its thumbnail's - which libfiles writes anew, making the EXIF
orientation agree, without encoding its picture again. Diffractor encodes AVIF, with libfiles' libheif and
libaom, but not HEIC - the HEVC encoder libheif takes, x265, is GPL: any other edit encodes an AVIF's
picture again, and saves a HEIC's as a JPEG of its own. A pixel write that produces an AVIF is encoded, as a
JPEG XL is, with its packet and the source's EXIF in the file's own items, and no update follows; the image's
rotation and mirror hold the orientation the file ends with, which a HEIF reader turns it by whatever the
EXIF says, and the EXIF is made to agree. A HEIF is decoded turned by its item's own rotation and mirror,
which the EXIF orientation a phone writes beside them describes again, so a copy made from one is written
upright with its EXIF orientation reset.

A pixel write in the file's own format keeps how the file was compressed: a JPEG is encoded against its own
quantization tables, a WebP stays lossless if it was, or is written at no less than the quality libwebp
estimates it had, and a JPEG XL stays lossless if it was, as does an AVIF coded as RGB itself, as a lossless
one is. Convert asks for its settings, so what it writes follows them instead. An animation - a
WebP, a PNG or a JPEG XL with frames, or a HEIF image sequence - shows one still: its first frame, or the
image a PNG or a HEIF keeps for a viewer that does not animate. Its picture is never written back over it,
which would keep that still alone: turning or editing one in place fails and leaves the file as it was,
while a copy made by Convert or Save As is made of that still.

A pixel edit loads its source as `decode_intent::edit` asks - at sixteen bits a channel where the file
holds more than eight: a 16-bit PNG, a HEIF or AVIF of ten or twelve bits, a JPEG XL of more than eight, or a
developed raw file - and is worked at that precision: PNG and JPEG XL are saved at sixteen bits, AVIF at ten, or
twelve for a twelve-bit source, and JPEG and WebP are rounded to a byte once, as they are encoded. Where the decode
budget holds only a byte a channel the edit is made at that, and the Edit panel says so. An HDR picture - PQ or
HLG - is loaded as its signal, with the description libfiles reads of its colour and light, which the AVIF, JPEG XL
or PNG it is saved as writes back. Its geometry is edited as the signal is, and its colour as the SDR rendition the
Edit view shows it as: libfiles' `carry_sdr_edit` scales the signal's light by what the adjustment did to the
rendition. Saved as a JPEG, it is that rendition with a gain map made for it; as a WebP, the rendition alone. One
too large to edit in HDR is edited as its rendition, which is never written back over it: Edit saves a change to its
pixels as a copy, and Rotate turning one it would have to re-encode fails and leaves the file as it was - a quarter
turn of a HEIF or a JPEG XL, written as an orientation, is still saved over it.

A picture in colours its ICC profile describes other than sRGB's - a phone's Display P3, a camera's Adobe RGB
(1998) - is loaded in those colours for an edit, which knows the profile (`picture_signal::profile`), and its colour
is edited as the sRGB rendition the Edit view shows: libfiles' `carry_srgb_edit` carries what the adjustment did to
the rendition back into the profile's colours, a pixel it left coming back exactly as it was, and the profile is
written with them, so a colour outside sRGB keeps what of its gamut the edit does not take.

A gain map - a JPEG's, Ultra HDR's, Adobe's, Apple's or ISO 21496-1's, or a HEIF's or AVIF's, ISO 21496-1's tone
map input or Apple's auxiliary image - says how much brighter each part of the photo is shown on an HDR display,
and goes with the photo: it is turned, straightened, corrected for perspective, cropped and scaled as the photo is,
at its own resolution and, in a JPEG, losslessly with a lossless turn where its blocks allow, and keeps its own
values, which scale whatever colour the edit gave the photo. A JPEG's is attached after the new photo, with the
multi-picture index and XMP signalling its metadata calls for; an AVIF's is written as an ISO 21496-1 tone map, so a
HEIC copied as an AVIF or a JPEG keeps it. PNG, JPEG XL and WebP hold none, and leave it out. A gain map that cannot
be edited is left behind, and the photo then says none follows. The metadata update after a re-encode keeps the
index and the XMP's account of the gain map true to the file. Edit offers a HEIC of more than eight bits, in HDR or
with a gain map as an AVIF rather than a JPEG, and a copy in a format that cannot hold the photo's HDR or its gain
map says so before it is written; Convert's formats say what each keeps.

Encoded JPEG, PNG, WebP, JPEG XL and AVIF buffers transfer ownership from libfiles
into the displayed image's blob without a second encoded-buffer copy.

Pasted and dropped pictures are saved alike, as PNG under an auto-numbered name: a PNG the clipboard or the drop
offers - as browsers, Office and the Snipping Tool do - is written as it came, its alpha included, and otherwise the
bitmap is encoded. Each is written to an owned temporary file - a paste's off the UI thread - and published to the
chosen destination only after the stream and encoder commit successfully.
The captured clipboard medium remains alive during encoding and is released on the apartment that
obtained it, including when worker execution fails or is cancelled. A copied picture is offered as a bitmap, a
device-independent bitmap and a PNG, each the sRGB rendition Diffractor shows; the PNG keeps its alpha.

Edit's lossless video and sound edits go through `files::update_av`, the same staging as an update:
`av_remux` copies the source's packets into the stage with FFmpeg's muxer for the format the destination
names - the file's own, or MP4 - trimmed, without the sound, or turned by the track's matrix, and nothing is
re-encoded; an MP4 made from another format starts where the source's timeline does. FFmpeg writes
back only the tags its demuxer read, so the stage is then given the source's XMP packet and the rating
and tags the item shows, where the copy's format keeps a packet; a format whose metadata is in a sidecar
keeps that sidecar, and Save As writes one beside a copy that cannot keep a packet. An MP4-family copy asked
for its index first, or whose source had it there, is rewritten last of all by `av_move_index_first`, its
movie box moved before its media with every chunk offset moved to match: last, because a metadata update
writes a movie box that has to grow whole at the end of the file. The stage replaces the destination, behind
a backup if requested, and is rescanned through its handle; the player reopens from that handle. A source
that cannot be read to its end fails the copy rather than saving the part before it, unless the copy is
repairing: then it reads on past what cannot be read, leaves out packets the muxer refuses, and reports
what it left out, and it replaces its own source only behind a backup. A save is cancelled up to the moment
the stage would replace the destination, so one cancelled while its tags are written still stops. A
cancelled or failed copy removes the stage and leaves the destination as it was.

A requested `.original` is created before replacement and never overwrites an existing backup.
Metadata commands do not imply a backup: callers request one explicitly.

## In-Place Metadata

In-place writing is allowed only when there is no path or pixel change and no backup request, for
every format libfiles writes, which carries the in-place trait: JPEG, PNG, GIF, WebP, TIFF and DNG -
one with no sidecar - JPEG XL, HEIC and AVIF, PSD, the MP4 and ASF families, WAV, AVI, MP3 and FLV. libfiles plans
the edit against the live file through one handle no one else may open meanwhile. An edit that fits
the space the file holds - a packet that keeps its length, EXIF values of the same size, ID3 padding,
the JUNK after a RIFF chunk, or what is new appended at the end, as a TIFF's directories and an MP4's
boxes are - is patched where the file lies: nothing the file holds moves, the writes go in flushed
steps ordered so that an interruption leaves a file that reads whole with each field old or new, and a
rewrite that keeps its length writes only the span it changes. The patched file keeps its identity,
named streams and attributes. An edit that does not fit is staged, as above.

An in-place write that loses a sharing race waits for an unlock and retries once. Other errors fail
immediately. Because no complete prior copy exists, the app cannot promise byte-for-byte rollback if
the admitted handler fails after changing the live file.

## Sidecars

Formats without embedded-XMP support use `<name>.xmp`. A DNG holds a packet of its own, but one with a
sidecar beside it - either name - is read from that sidecar, so its edits are written there too: Camera
Raw's rule for a DNG unless told to ignore sidecars. A DNG without one is written itself, as Lightroom
always writes one; only a sidecar known not to exist sends an edit into the DNG. A metadata-only edit
to a raw's sidecar changes only the staged sidecar; the media bytes remain untouched. When creating the
first RAW sidecar, the update starts from the effective source packet so embedded XMP is not shadowed
by an otherwise default sidecar. An existing sidecar, including an explicitly empty one, remains the
higher-priority source; an unreadable sidecar fails rather than being replaced. For a combined media
and sidecar update, the media replacement completes first and is rolled back if the sidecar
replacement fails where the platform supports that recovery.

Import treats automatic sidecars as companions, not additional rows. When collision resolution sends
two primaries sharing one sidecar to different basenames or folders, each destination receives the
sidecar. A completed companion copy is reused only by the same source and destination pair and is
revalidated before reuse. Move retains the source sidecar until every dependent primary has moved;
skipped, failed, and canceled sources keep their metadata.

## Coherent Read-Back

Every successful write is rescanned so the index, thumbnail, and displayed properties describe the
new bytes. A staged replacement keeps a handle to the staged file through its rename and rescans
through that handle. This avoids a stale by-name SMB cache returning the old destination after the
swap. If a filesystem cannot provide the coherent-handle path, the fallback replaces by name and the
result is marked accordingly.

Self-authored metadata-only changes publish the new metadata and modified time while retaining valid
pixels. Pixel or presentation changes invalidate the display ladder. External changes are discovered
by folder comparison and treated conservatively because their effect is unknown.

## Failure And Recovery

| Failure | Outcome |
| --- | --- |
| Stage or staged metadata cannot be produced | Live destination is unchanged; temporary files are removed |
| Requested backup cannot be created | Operation fails before replacement |
| Media replacement fails | Destination remains unchanged |
| Sidecar replacement fails | Media is restored when a complete rollback copy exists |
| In-place patch fails | One lock retry, then a reported failure and rescan; no full rollback promise |

Cancellation stops future items in a batch and reports completed, failed, skipped, and cancelled
items separately. It does not claim to undo completed writes.

Rename recovers a displaced, unfinished media file and all its sidecars as one group. It uses their
original names if available, otherwise their reviewed names, otherwise one collectively free numbered
basename. Recovery never places a sidecar beside an unrelated primary; a failed recovery reports the
surviving paths.

## Where this lives

- [files.cpp](../src/files.cpp): `files::load`, `files::update`, `files::update_av`, staging, sidecars,
  and write result publication, plus archive listing; `files::with_edited_gain_map`, which edits a
  JPEG's gain map as its photo was and attaches it; and `source_gain_map` and `gain_map_for`, the gain map a
  re-encoded photo is written with - its own, edited, or one made for an HDR picture a JPEG holds.
- [media_edit.cpp](../src/media_edit.cpp): `apply_image_edits`, which edits an HDR picture's colour as its SDR
  rendition is shown edited, and a profile's colours as their sRGB rendition is.
- [view_edit.cpp](../src/view_edit.cpp): `edit_offers_avif`, `edit_save_loss` and `edit_photo_note`, what Edit
  offers and says of a photo's depth and range.
- [av_format.cpp](../src/av_format.cpp): `av_remux`, the lossless copy of a video or a sound file, and
  `av_plan_edits`, what that copy can do and leaves out.
- [util_zip.cpp](../src/util_zip.cpp): ZIP creation, entry names, per-entry ZIP64 headers, and entry
  timestamps.
- [files.cpp](../src/files.cpp)'s `s_still_formats`: the still image formats recognised by signature,
  one entry each for detection, header scanning, loading, and decoding.
- [model.cpp](../src/model.cpp), [model.h](../src/model.h) and [model_display.h](../src/model_display.h):
  `texture_state` and `display_state_t`.
- [model_index.cpp](../src/model_index.cpp): change discovery, scanning, and thumbnail scheduling.
- libdesktop's [files.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/platform/win/files.cpp):
  replacement, coherent file handles, and the pasted clipboard bitmap's encoding and staged
  publication.
- [view_rename.cpp](../src/view_rename.cpp): displaced rename groups and cancellation recovery.
- [app_util.cpp](../src/app_util.cpp): Import planning, shared companions, and write rollback.
- [metadata.cpp](../src/metadata.cpp): XMP packet and sidecar updates.