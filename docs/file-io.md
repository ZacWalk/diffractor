# File I/O

This document owns the path from media bytes to displayed pixels, media and sidecar writes, coherent
read-back, and failure outcomes. [Metadata](metadata.md) owns property-to-tag mappings;
[Rendering](rendering.md) owns texture upload; [Zoom](zoom.md) owns scale and center; and
[Design](design.md) owns user-facing operation semantics.

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
recreate them. GPU textures are created lazily by the UI-owned draw context and may be discarded on
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

Pasted clipboard bitmaps follow the same publication rule: the encoder writes an owned temporary
file off the UI thread, then publishes it to the chosen auto-numbered destination only after the
stream and encoder commit successfully.
The captured clipboard medium remains alive during encoding and is released on the apartment that
obtained it, including when worker execution fails or is cancelled.

A requested `.original` is created before replacement and never overwrites an existing backup.
Metadata commands do not imply a backup: callers request one explicitly.

## In-Place Metadata

In-place writing is allowed only when there is no path or pixel change and no backup request, for
every format libfiles writes, which carries the in-place trait: JPEG, PNG, GIF, WebP, TIFF and DNG,
PSD, the MP4 and ASF families, WAV, AVI, MP3 and FLV. libfiles plans the edit against the live file
through one handle no one else may open meanwhile. An edit that fits the space the file holds - a
packet that keeps its length, EXIF values of the same size, ID3 padding, the JUNK after a RIFF chunk,
or what is new appended at the end, as a TIFF's directories and an MP4's boxes are - is patched where
the file lies: nothing the file holds moves, the writes go in flushed steps ordered so that an
interruption leaves a file that reads whole with each field old or new, and a rewrite that keeps its
length writes only the span it changes. The patched file keeps its identity, named streams and
attributes. An edit that does not fit is staged, as above.

An in-place write that loses a sharing race waits for an unlock and retries once. Other errors fail
immediately. Because no complete prior copy exists, the app cannot promise byte-for-byte rollback if
the admitted handler fails after changing the live file.

## Sidecars

Formats without embedded-XMP support use `<name>.xmp`. A metadata-only RAW edit changes only the
staged sidecar; the media bytes remain untouched. When creating the first RAW sidecar, the update
starts from the effective source packet so embedded XMP is not shadowed by an otherwise default
sidecar. An existing sidecar, including an explicitly empty one, remains the higher-priority source;
an unreadable sidecar fails rather than being replaced. For a combined media and sidecar update, the
media replacement completes first and is rolled back if the sidecar replacement fails where the
platform supports that recovery.

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

- [files_core.cpp](../src/files_core.cpp): `files::load`, `files::update`, staging, sidecars, and
  write result publication, plus archive listing.
- [util_zip.cpp](../src/util_zip.cpp): ZIP creation, entry names, per-entry ZIP64 headers, and entry
  timestamps.
- [files_formats.cpp](../src/files_formats.cpp): the still image formats recognised by signature,
  one entry each for detection, header scanning, loading, and decoding.
- [model.cpp](../src/model.cpp), [model.h](../src/model.h) and [model_display.h](../src/model_display.h):
  `texture_state` and `display_state_t`.
- [model_index.cpp](../src/model_index.cpp): change discovery, scanning, and thumbnail scheduling.
- libdesktop's [platform_win_files.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/system/platform_win_files.cpp): replacement and coherent file handles.
- [view_rename.cpp](../src/view_rename.cpp): displaced rename groups and cancellation recovery.
- [app_util.cpp](../src/app_util.cpp): Import planning, shared companions, and write rollback.
- libdesktop's [platform_win_wic.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/render/platform_win_wic.cpp): pasted clipboard bitmap encoding and staged
  publication.
- [metadata_xmp.cpp](../src/metadata_xmp.cpp): XMP packet and sidecar updates.