# Metadata

This document owns the mapping between Diffractor properties and media metadata standards.
[Design](design.md) owns what metadata commands mean; [File I/O](file-io.md) owns staging, sidecars,
backups, and failure; [Third-party](third-party.md) owns libfiles, which reads and writes the packets, and the DNG SDK's XMP
backend.

Media files remain authoritative. The index stores derived property snapshots for search and display.

## Containers

| Container | Read | Write |
| --- | --- | --- |
| JPEG, TIFF | EXIF, IPTC, XMP | XMP reconciled to EXIF/IPTC where supported |
| PNG, WebP | EXIF/XMP where present | XMP |
| RAW photos | EXIF and embedded/sidecar XMP | Sidecar XMP |
| JPX, WBMP | Sidecar XMP | Sidecar XMP |
| MP4, MOV, M4A | QuickTime/iTunes tags and XMP | XMP, Windows tags and rating, movie header dates, ISO copyright |
| MP3, MP2, MPA | ID3v2 and XMP | XMP reconciled to ID3v2 |
| AVI, WAV | RIFF INFO and XMP | XMP reconciled to RIFF INFO |
| ASF, WMV, WMA | Windows Media attributes and XMP | XMP reconciled to Windows Media attributes |
| FLV | `onMetaData` and XMP | XMP |
| HEIC, AVIF | EXIF and XMP | Not editable |
| MKV, WebM | Matroska tags | Not editable |

Read and write support are intentionally asymmetric. FFmpeg exposes more container tags than the
writers reconcile, so displaying a property does not prove it can be stored back into that
container. Per-format edit traits gate commands before the write path runs.

## Property Mapping

| Diffractor property | XMP | Photo metadata | Audio/video metadata |
| --- | --- | --- | --- |
| Title | Dublin Core title | IPTC Object Name | ID3 title, RIFF title, QuickTime title |
| Description | Dublin Core description | EXIF ImageDescription, IPTC Caption | ID3 comment, RIFF comment, container description |
| Comment and synopsis | XMP comment/description fields | EXIF UserComment where present | ID3 comment, long description |
| Tags | Dublin Core subject | IPTC keywords, Windows XP keywords | ID3 genre/keyword fields, RIFF keywords |
| Rating | `xmp:Rating` | EXIF/Windows rating fields | ID3 POPM, Windows shared rating, Matroska rating on read |
| Label | `xmp:Label` | XMP only | XMP only |
| Artist/creator | Dublin Core creator | EXIF Artist, IPTC By-line | ID3 artist, QuickTime artist, RIFF artist |
| Album/track/disc | Dynamic Media fields | n/a | ID3 and QuickTime album/track/disc fields |
| Copyright | Dublin Core rights | EXIF Copyright, IPTC CopyrightNotice | ID3, RIFF, ASF, and ISO copyright fields |
| Location text | IPTC Core location fields | IPTC/XMP city, state, country | XMP where the container supports it |
| Coordinate | EXIF GPS and XMP location | EXIF GPS latitude/longitude/altitude | ISO6709 location where exposed |

String alternatives are normalized into one property snapshot. XMP generally has precedence over
legacy fields because it is the representation Diffractor writes; format-specific readers retain
legacy values when no higher-priority value exists. Ratings are normalized to zero through five.
The QuickTime `rtng` atom is a content-advisory flag, not a star rating.
For tags, an empty XMP `dc:subject` is an explicit higher-priority empty list; an absent
`dc:subject` does not suppress legacy or container keywords.

## Dates

Diffractor exposes three date concepts:

| Date | Meaning | Typical sources |
| --- | --- | --- |
| Original | When the depicted or recorded content was made | EXIF DateTimeOriginal, XMP CreateDate, GPS date/time |
| Created | When this media file or encoding was made | EXIF DateTimeDigitized, container creation time, filesystem creation |
| Modified | When the file last changed | XMP ModifyDate, container modification time, filesystem modification |

The scanner retains candidate values with source identity and precision, then resolves one answer per
concept using the precedence in `model_dates.h`. A timezone-qualified instant outranks an otherwise
equivalent floating local value, but the pack keeps incompatible representations distinct: a UTC
instant and a floating wall-clock reading that display alike on the current machine remain separate
candidate values. Invalid, placeholder, and out-of-range dates are ignored.

UTC/local conversion uses the timezone rules for the timestamp's date, not today's daylight-saving
offset, and preserves the timestamp's fractional precision. Ambiguous or skipped local times follow
the operating system's timezone rules. Each year's offsets are taken from the operating system once
per time zone and reused, so converting every item's date stays cheap. Filesystem creation is a
birth-time fact; inode-change time is not substituted when a filesystem cannot supply it.

The date pack is self-describing: readers consume fields they understand and skip later additions.
Source-bit meanings are never reused. Compatibility fields permit older builds to recover useful
dates when they encounter a newer cache row; the cache remains disposable and may be rescanned.

Writes update the standard matching the property and container. File-system dates are not silently
substituted for authored metadata during a metadata save.

## Photo Metadata

EXIF supplies camera, lens, exposure, dimensions, orientation, resolution, color space, GPS, and
capture dates. IPTC supplies editorial title, caption, creator, copyright, location text, and
keywords. JPEG stores IPTC in Photoshop image resources, so resource order, Pascal names, odd
payload padding - a last resource may lack its pad byte - and unrelated resources do not change
the effective IPTC payload; of IPTC resources a file repeats, the first that holds anything is the
one read and the one an edit changes. IPTC text is
UTF-8 where it is valid and Windows-1252 where it is not, as Photoshop and the wire services wrote
it, so an older caption's curly quotes and dashes read as written. XMP can override
or extend both and carries Diffractor's editable rating, label, tags, description, and location
values.

Orientation affects presentation and is therefore not treated as display-neutral metadata. A pixel
edit draws the stored orientation into the pixels it writes, so saving one resets the orientation to
upright rather than leaving the tag to turn the saved image a second time. The EXIF orientation is
the last Orientation in IFD0, else the last in IFD1, which describes the thumbnail; a copy of the tag
in any other directory is not the image's. The item, the scan and every decoder read it by that one
rule, through libfiles, so a file is never shown turned one way and its thumbnail another. Embedded
thumbnails and ICC profiles are parsed with the image and invalidated when their source metadata
changes.
Stored image dimensions follow transformed pixels, including lossless JPEG rotation, which sets the
EXIF orientation upright and the dimensions where the block holds them - every copy, where a block
repeats a tag, so it reads back upright whichever copy a reader takes - and changes nothing else in
it, maker notes included.

Unknown tags are not rewritten merely because Diffractor does not display them. XMP updates begin
from the existing packet. For a photo, each field an edit sets is also written to the EXIF and IPTC
fields that hold it - title to IPTC Object Name; description to EXIF ImageDescription and IPTC
Caption; tags to IPTC Keywords and Windows XP keywords; rating to the EXIF and Windows rating fields;
creator, copyright, dates, GPS, orientation and location text likewise - and nothing else in either
block changes. IPTC is created when a field it holds is written, and is stored as UTF-8. A tag edit
on a packet without `dc:subject` starts from the tags the item shows, legacy keywords included.
An MP4 or QuickTime edit writes tags and rating to the Xtra box Windows reads as well, a copyright to
the ISO `cprt` box and a created date to the movie header; the packet always follows the movie box,
since FFmpeg reads no XMP box before it. An ASF edit writes title, author, copyright and description
to the header's Content Description, tags and rating to the WM/Category and WM/SharedUserRating
descriptors, and a created date to the File Properties. An MP3 edit writes the ID3v2 text frames
players read - title, artist, album, album artist, genre, comment, track, disc, copyright and date - and
a rating to Windows Media Player's POPM frame, keeping the tag's version and every frame the edit does
not own. A WAV or AVI edit writes title, artist, comment, copyright, date and genre to LIST INFO, an
AVI's INFO before `movi` and its packet after it, where FFmpeg reads them. An FLV keeps its packet in an
`onXMPData` tag after `onMetaData`. Only the fields an edit sets are written: a native field the packet
lacks is left as it is.

## Audio And Video Metadata

FFmpeg container dictionaries are normalized into shared properties. Language-suffixed comment and
description keys are selected by plain value, UI language, then any available value. Container
duration, streams, codecs and their profiles, bitrate, frame rate, pixel/audio format, bit depth and
HDR transfer are technical properties and are not written through the user metadata editor.

A video's shape also comes from the container rather than from its frames: the sample aspect ratio
gives the displayed dimensions for anamorphic video, and the display matrix gives the orientation.
Both are read without decoding, so a scan that never opens a frame describes the same shape the
player does.

Writes reconcile a bounded legacy subset. In particular, MP4/MOV writes do not rewrite the
full iTunes atom set, so software that ignores XMP may continue to show an older atom value.
Matroska metadata is read-only.

## Current Limitations

- HEIC, AVIF, JXL, MKV, and WebM do not expose editable metadata traits.
- MP4/MOV legacy iTunes atoms are broader on read than on write.
- ID3 POPM ratings are recognized only for supported owner conventions.
- A fixed number of date candidates is retained; excess sources remain marked present but may not
  participate in resolution.

TIFF camera, lens, exposure and date fields are read from the file's own
directories by the same EXIF parser used for embedded blocks. Its dates enter
the normal EXIF date-resolution rules; IPTC and XMP retain their existing
precedence. The scan does not copy TIFF pixel data into a metadata block.

## Panorama

An image is a panorama only when GPano metadata declares it. `ProjectionType` maps to
equirectangular, cylindrical, unspecified panorama, or none. Other GPano fields can establish an
unspecified panorama when projection type is absent. Aspect ratio alone never declares one.

Full panorama dimensions and crop offsets are read when the item is displayed. The crop must agree
with decoded dimensions and remain inside the declared full panorama; otherwise projection geometry
is discarded. These values feed [zoom.md](zoom.md#panorama) and are not written as derived metadata.

Embedded geometry is read from HEIF/AVIF and JPEG XL too, including compressed
JPEG XL XMP boxes. A valid embedded declaration wins; otherwise a sidecar is
read on its own, never combined with an incomplete embedded declaration.

`@panorama` and `@pano` search the stored projection classification and canonicalize to one term.

## Where this lives

- libdesktop's [platform_win.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/system/platform_win.cpp) and [platform_linux.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/system/platform_linux.cpp):
  date-sensitive conversion between UTC and local wall time.

- [model_dates.h](../src/model_dates.h): date sources, retained candidates, and resolution.
- [metadata_exif.cpp](../src/metadata_exif.cpp), [metadata_iptc.cpp](../src/metadata_iptc.cpp), and
  [metadata_xmp.cpp](../src/metadata_xmp.cpp): standard-specific mapping and reconciliation. EXIF is
  read by libfiles' `read_exif`, the one parser the scan and the decoders use too;
  `metadata_exif.cpp` maps it onto an item, writes EXIF through libfiles - a fresh block with
  `make_exif`, a turned picture's numbers with `set_exif_numbers` - and lists it with
  `list_exif`, which reads as libexif's listing did. IPTC is read by libfiles' `read_iptc` and
  listed by its `list_iptc`; `metadata_iptc.cpp` maps the datasets onto an item after EXIF and
  before XMP, combining the date and time datasets and merging keywords and bylines with what EXIF
  supplied.
- [av_format.cpp](../src/av_format.cpp): audio/video container metadata.
- [model_property.cpp](../src/model_property.cpp): shared property identity and formatting.
- [model_db_pack.h](../src/model_db_pack.h) and [model_db.cpp](../src/model_db.cpp): cached metadata
  serialization.
- [files_core.cpp](../src/files_core.cpp) and [files_scan_photo.cpp](../src/files_scan_photo.cpp):
  format dispatch and scan publication. Every still - JPEG, PNG, WebP, GIF, TIFF, PSD, BMP, HEIF and
  AVIF, and JPEG XL - is scanned by [libfiles](https://github.com/ZacWalk/libfiles), whose result
  `files_scan_photo.cpp` maps onto a `file_scan_result`; libfiles decodes a HEIF thumbnail item a scan
  wants, and [files_jxl.cpp](../src/files_jxl.cpp) supplies the Brotli a JPEG XL's compressed boxes need.