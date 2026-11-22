# Metadata

This document owns the mapping between Diffractor's properties and the metadata standards files keep
them in. [Design](design.md) owns what metadata commands mean; [File I/O](file-io.md) owns staging,
sidecars, backups and failure; [Third-party](third-party.md) owns libfiles - which reads and writes the
blocks, packets and container tags, and whose
[design](https://github.com/ZacWalk/libfiles/blob/main/docs/design.md) gives each format's exact fields -
and the DNG SDK's XMP backend.

Media files remain authoritative. The index stores derived property snapshots for search and display.

## Containers

| Container | Read | Write |
| --- | --- | --- |
| JPEG, TIFF, PSD | EXIF, IPTC, XMP | XMP, and the EXIF and IPTC fields an edit sets |
| PNG, GIF | XMP, and a PNG's EXIF | XMP |
| WebP | EXIF and XMP where present | XMP, and the EXIF it holds |
| HEIC, AVIF | EXIF and XMP | XMP and the EXIF it holds; a turn in its image's rotation and mirror |
| JPEG XL | EXIF and XMP boxes, Brotli-compressed or not | XMP and the EXIF it holds; a turn in its codestream's orientation |
| Camera raw | EXIF, and embedded or sidecar XMP | Sidecar XMP |
| DNG | EXIF and XMP, or its sidecar's XMP when it has one | Its sidecar when it has one, otherwise the DNG |
| JPX, WBMP, and MPEG streams (MPG, VOB, MP2 and kin) | Sidecar XMP | Sidecar XMP |
| MP4, MOV, M4A | QuickTime and iTunes tags, XMP | XMP; tags and rating in the Xtra box Windows reads, the created date in the movie header, copyright in `cprt` |
| MP3, MPA | ID3v2, XMP | XMP, and the ID3v2 text frames and POPM rating an edit sets |
| WAV, AVI | RIFF INFO, XMP | XMP, and the LIST INFO fields an edit sets |
| ASF, WMV, WMA | Windows Media attributes, XMP | XMP, and the Content Description, WM/Category, WM/SharedUserRating and File Properties fields an edit sets |
| FLV | `onMetaData`, XMP | XMP |
| MKV, WebM, FLAC, Ogg, AIFF | Their tags, and an AIFF's or FLAC's XMP | Not editable |

A converted or saved AVIF or JPEG XL is encoded with its EXIF and XMP ([File I/O](file-io.md#writing)).
Read and write are deliberately asymmetric: libfiles reads more container tags than the writers
reconcile, so displaying a property does not prove it can be stored back. A file type's traits
(`file_traits::edit`, `embedded_xmp`) gate commands before the write path runs.

## Property Mapping

| Diffractor property | XMP | Photo blocks | Audio and video tags |
| --- | --- | --- | --- |
| Title | `dc:title` | IPTC Object Name, EXIF XPTitle | ID3, RIFF, QuickTime, ASF and Matroska title |
| Description | `dc:description` | EXIF ImageDescription (else XPSubject), IPTC Caption | MP4 description, RIFF ISBJ, Matroska DESCRIPTION |
| Comment | `xmpDM:logComment`, read else from `exif:UserComment` | EXIF UserComment (else XPComment) | ID3, RIFF and QuickTime comment; ASF description |
| Synopsis | `xmpDM:synopsis` | - | MP4 `ldes`, Matroska SYNOPSIS |
| Tags | `dc:subject`, with `exif:XPKeywords` written beside it | IPTC Keywords, EXIF XPKeywords | MP4 and QuickTime keywords, RIFF IKEY, Matroska KEYWORDS, Windows' WM/Category |
| Rating | `xmp:Rating`, with `MicrosoftPhoto:Rating` written beside it | EXIF Rating and RatingPercent | ID3 POPM, WM/SharedUserRating, Matroska RATING |
| Label | `xmp:Label` | - | - |
| Artist | `xmpDM:artist` | EXIF Artist (else XPAuthor), IPTC By-lines | ID3, RIFF and QuickTime artist; ASF author |
| Creator | `dc:creator` | Written to EXIF Artist and IPTC By-lines | Written as ASF's author where no artist is set |
| Album, album artist, genre, track, disc, show, season, episode | `xmpDM:` of each | - | ID3, QuickTime and iTunes, RIFF and Matroska fields of each |
| Copyright | `dc:rights`, its URL `xmpRights:WebStatement` | EXIF Copyright, IPTC Copyright Notice | ID3, RIFF, ASF and ISO `cprt` copyright |
| Credit, source | `photoshop:Credit`, `photoshop:Source` | IPTC Credit, Source | - |
| Location text | `photoshop:City`, `State`, `Country` | IPTC City, Province/State, Country | - |
| Coordinate | `exif:GPSLatitude`, `GPSLongitude` | EXIF GPS, with altitude and speed | ISO 6709 location |
| People, and where their faces are | `mwg-rs` face regions, else Microsoft's regions; `Iptc4xmpExt:PersonInImage` | - | - |

A dash means XMP only. People and their face regions are [faces](faces.md)' to describe: which schema is
read when and how a region's area is turned upright ([§2](faces.md#2-people-the-files-name)), and how a
name is written into a region when a person is renamed ([§10](faces.md#10-saving-names-to-files)).

A still's EXIF is read, then its IPTC over it, then its packet over both; a recording's container tags,
then a media manager's `.nfo` ([below](#films-episodes-and-music)), then its packet. What a later block
states replaces what earlier ones gave - and a packet states a property by holding it at all, even
empty - except that IPTC's keywords and by-lines join EXIF's. So an empty `dc:subject` is an explicit
empty tag list, while an absent one leaves legacy and container keywords in place. Ratings are stars
from 1 to 5, 0 for none and -1 for rejected, as `xmp:Rating` has them; Windows' percent scales are
converted, and a POPM rating is read from Windows Media Player's frame, else the first, on Windows Media
Player's scale. The QuickTime `rtng` atom is a content advisory, not a rating.

## Dates

Diffractor shows three dates, each answered by the highest-ranked source a file gives (`date_sources` in
[model_dates.h](../src/model_dates.h)):

| Date | Meaning | Sources, highest first |
| --- | --- | --- |
| Original | When the content was made | EXIF DateTimeOriginal, `exif:DateTimeOriginal`, IPTC Date Created, an ID3 original release date, a media manager's `.nfo`, a film's or episode's name, `photoshop:DateCreated`, the Windows shell's date taken for a cloud file not downloaded |
| Created | When this file or encoding was made | EXIF DateTimeDigitized, `exif:DateTimeDigitized`, IPTC Digital Creation Date, `xmp:CreateDate`, the container's creation time, a rip date, the filesystem's creation time |
| Modified | When the file last changed | The filesystem's modification time, then EXIF DateTime, then `xmp:ModifyDate` |

The GPS time stamp is read beside them but answers none of the three. Where one date is shown, it is
Original, else Created, else Modified.

A file's dates are kept with the tags they were read from, sources that agree sharing one value, up to
four distinct values; a further source's tag is recorded but cannot answer. A value keeps its form: a
UTC instant - a container or filesystem stamp, converted when shown - or a wall-clock reading with or
without an offset. Readings of one wall-clock second merge, keeping a stated offset and the finer
fraction, but a UTC instant and a floating reading that display alike on this machine stay distinct.
Invalid, placeholder and out-of-range dates are ignored.

UTC/local conversion uses the time-zone rules of the timestamp's own date, not today's offset, keeps its
fraction, and follows the operating system for ambiguous or skipped local times; each year's rules are
taken from the operating system once per zone and reused. A filesystem creation time is a birth time:
where a filesystem has none, inode-change time is not substituted.

The cached date pack is self-describing: readers take the fields they understand and skip later ones, a
source bit's meaning is never reused, and compatibility fields let older builds recover useful dates
from a newer row; the cache stays disposable.

An edited Original is written to `photoshop:DateCreated` and `exif:DateTimeOriginal`, and an edited
Created to `xmp:CreateDate` and `exif:DateTimeDigitized` - both, so the file's own EXIF cannot outrank
the edit - and to the format's own date fields where it keeps them. A metadata save never writes a
filesystem date as authored metadata.

## Photo Metadata

EXIF gives camera, lens, exposure, dimensions, orientation, resolution, colour space, GPS and capture
dates; IPTC editorial title, caption, creator, copyright, location text and keywords; XMP overrides or
extends both and carries Diffractor's edits. libfiles reads each block as its
[Metadata blocks](https://github.com/ZacWalk/libfiles/blob/main/docs/design.md#metadata-blocks) says: a
JPEG's IPTC from its Photoshop resources, where the first IPTC resource that holds anything is the one
read and the one an edit changes; IPTC text as UTF-8 where it is valid and Windows-1252 where not, so an
older caption's curly quotes and dashes read as written; and a TIFF's own directories by the same EXIF
parser, without copying its pixels into a metadata block.

Orientation is not display-neutral. The EXIF orientation is the last Orientation in IFD0, else the last
in IFD1; the item, the scan and every decoder read it by that rule through libfiles, so a file is never
shown turned one way and its thumbnail another. A pixel edit draws the orientation into the pixels it
writes and resets it to upright. A lossless JPEG turn sets the orientation upright and the dimensions in
every copy the block holds, and changes nothing else in it, maker notes included. Embedded thumbnails
and ICC profiles are parsed with the image and invalidated when their source changes.

An edit starts from the file's existing packet and writes only the fields it sets - into the packet and
wherever else the format keeps them ([Containers](#containers)) - so tags Diffractor does not display
are never rewritten, and a native field the edit does not set is left as it is. IPTC is created only
when a field it holds is written, and is written as UTF-8. libfiles'
[How it writes](https://github.com/ZacWalk/libfiles/blob/main/docs/design.md#how-it-writes) gives each
format's fields and where they go. A tag edit on a packet without `dc:subject` starts from the tags the
item shows, legacy keywords included. A conversion writes the source's packet - or, for a file libfiles
cannot update, such as a camera raw without a sidecar, the packet its scan finds - and fills what the
packet lacks from what the item shows, since the new format may have no place for the source's IPTC.

## Audio And Video Metadata

libfiles' media scan reads every container libfiles knows - MP4 and QuickTime, Matroska and WebM, AVI,
WAV, AIFF, ASF, MPEG audio, FLAC, Ogg, FLV, MPEG-TS and MPEG-PS - without decoding: streams, duration,
tags, XMP packet and cover art. Tags fill the shared properties by the names FFmpeg's demuxers give
them, so a recording files where it did, and the Windows tags and ratings libfiles writes (MP4's Xtra
box, ASF's WM/Category and WM/SharedUserRating, MP3's POPM) read back from where they were written. A
file libfiles cannot describe whole has its streams described by FFmpeg, and its tags too when libfiles
read none. MP4 and MOV writes leave the iTunes atoms as they are, so software that reads neither the
packet nor the Xtra box may still show an older value.

The cover art is the front cover, else the first picture of the recording; an icon, or the picture ID3
and FLAC call "a bright coloured fish" - the type a store embeds its drawing of the track's waveform
under - is never taken for it, by the scan or by the player. One comment is shown: an untagged one, else
one in the UI language, else any; where FFmpeg reads the tags every text field is chosen so, which reads
a tag a Matroska file states only in another language than its default (`TITLE-ger`). The info panel
lists every tag under the name the container stores it by, beside the container's structure.

A date keeps the zone the file states: a creation date with its offset, as Apple's cameras write it, is
the time the camera's clock showed, as a photo's EXIF time with its offset is; a UTC instant, as an MP4
movie header holds, is converted for display; and a date alone, as an MP3's ID3 date often is, is that
day wherever it is shown. A tag giving only a year (`©day` 1998, `TYER`) dates its track or film to that
year.

Duration, streams, codecs and their profiles, bitrate, frame rate, pixel format, audio sample type, bit
depth and HDR transfer are technical properties, never written through the metadata editor, and named
in Diffractor's terms whichever library read them: codecs by short name (`h264`, `mpeg2`, `dv`, `pcm`),
a pixel format as chroma with any depth past 8 bits (`yuv420`, `yuv422 10bit`), and the sample type as
the stream codes its samples - `16bit`, `24bit`, `float` for PCM and lossless audio, none for a lossy
codec.

A video's shape also comes from the container, not its frames: the sample aspect ratio gives the
displayed dimensions of anamorphic video, and the display matrix its orientation - its turn, and its
mirror where the transform reflects the picture, numbered as EXIF numbers an orientation - so a scan
that never opens a frame describes the shape the player shows.

## How A Picture Is Coded

How a picture or video is coded is read from the file alone, indexed for search, and never written: its
colour profile - its ICC profile's own name, else the primaries its coded colour description names
(`sRGB`, or `BT.709` for video; `BT.601`, `BT.2020`, `DCI-P3`, `Display P3`), and none for a file that
states neither, rather than the sRGB it most likely is; its bit depth; a video's frame rate; whether it
is HDR - coded with PQ or HLG, or a photo carrying a gain map (Ultra HDR, ISO 21496-1 or Apple's); and
whether a picture is the first frame of an animation or has an alpha channel. `color:`, `bits:`, `fps:`
and `hdr:` (`hdr:hlg`, `hdr:pq`, `hdr:"gain map"`) search them; a whole frame rate finds what rounds to
it, so `fps:30` finds 29.97 too.

## Commodore 64 Images

A disk, tape, cartridge or program image is described by what the C64 lists of it: the name it gives
itself as its title - a disk's header, a tape's, a cartridge's - `Commodore 64` as its system, as a SID
tune's is, and the names of the files it holds, one a line, as its contents. Search reads every name,
and `contents:` asks for an image holding a file of one name; the vocabulary offers the first 64 names
of each image.

## Films, Episodes And Music

A video's name says what most downloads are: a film's title and year (`Dune.2021.2160p.WEB-DL`), an
episode's show, season and number (`Show.S01E02`, `1x02`, `S01E01E02`, a special as `S03E00`, a dated
episode as `Show.2024.01.07`), and the release tags around them. These are read as the item loads and
fill only what the video's own tags leave out; a container title or comment that is only a release
name, a website or the file's own name counts as absent. An episode whose name leaves out its show
takes it from its folders: `Show/Season 2/S02E05.mkv`. Values read from names are marked and never
stored, so a better reading of names needs no rescan.

What a collection folder holds, chosen in [Options](design.md#options), decides what its videos are:

| Folder holds | A video is |
| --- | --- |
| Decided by each file's name (default) | An episode when its name numbers one; a film when its name carries a release tag and a year |
| Films and TV shows | An episode when its name numbers one; otherwise a film |
| Personal recordings, Music | Never a film or an episode, whatever its name |

A film files under its release year and an episode under the day it aired, read from the name, when the
file's own dates name another year or day, so a download does not file under the day it was copied.
Such a date shows as "File name" among the item's dates.

An MP4 marked a film (`stik` 9) is never an episode, whatever its `tvsn` and `tves` say. Matroska tags
that name their level give a show, season and episode: `COLLECTION/TITLE`, `SEASON/PART_NUMBER` and
`EPISODE/PART_NUMBER`.

With Options' .nfo setting on, the `.nfo` a media manager - Kodi, Jellyfin, Emby - keeps beside a video
gives its title, plot, genres, studio, year, show, season, episode and premiere or air date. That is its
own `Movie.nfo`, or a film folder's `movie.nfo` when the film is named for its folder, with the show from
the `tvshow.nfo` in a season's folder or the one above. Its values are set over the container's tags and
under the file's XMP, a date read from one shows as "Media manager .nfo", and a film it describes is a
film whatever its name. A release group's text `.nfo` is not XML and reads as nothing. `.nfo` files,
like subtitles, travel with their video as sidecars.

A track or film with no art of its own takes the art kept beside it: an album's `Folder.jpg`,
`cover.jpg` or `front.jpg`, and a video's `Movie-poster.jpg`. A folder's own `poster.jpg` or `folder.jpg` is
a film's only when the folder holds that film alone, named for it as media managers lay films out
(`Dune (2021)/Dune (2021).mkv`): beside home videos, many films, or a season's episodes, folder art would
make every video look the same.

## Panorama

An image is a panorama only when GPano metadata declares it: `ProjectionType` maps to equirectangular,
cylindrical, an unspecified panorama, or none, and other GPano fields declare an unspecified one when it
is absent. Aspect ratio alone never declares one. Full dimensions and crop offsets are read when the item
is displayed; a crop that disagrees with the decoded dimensions or leaves the declared panorama discards
the projection. A valid embedded declaration - read from HEIF, AVIF and JPEG XL too, compressed boxes
included - wins; otherwise a sidecar's is read on its own, never joined to an incomplete embedded one.
These values feed [zoom.md](zoom.md#panorama) and are never written back.

`@panorama` and `@pano` search the declared projection and canonicalize to one term. `@wide` asks the
shape instead: anything shown at least twice as wide as it is tall, declared or not.

## Where this lives

- libfiles' [properties.h](https://github.com/ZacWalk/libfiles/blob/main/include/libfiles/properties.h):
  the one table each block is read by, marking what it states, and the one rule reconciling them.
- [files.cpp](../src/files.cpp)'s `file_scan_result::to_props` and `apply_properties`: what an item takes
  from a still's EXIF and IPTC, a recording's tags, a media manager's `.nfo` and the packet, in that
  order; `apply_properties` is the one place libfiles' properties meet an item.
- [metadata.cpp](../src/metadata.cpp): writing and listing. `metadata_edits::apply` maps an edit into
  the packet and `legacy_edits_of` into the fields outside it; `metadata_xmp` reads and writes packets
  and sidecars; `metadata_exif` writes EXIF through libfiles - a fresh block with `make_exif`, a turned
  picture's numbers with `set_exif_numbers` - and lists it with `list_exif`; `metadata_iptc` lists IPTC
  with `list_iptc`.
- [model_dates.h](../src/model_dates.h): date sources, their ranking (`date_sources`), and the date pack.
- libdesktop's [win/platform.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/platform/win/platform.cpp)
  and [linux/platform.cpp](https://github.com/ZacWalk/libdesktop/blob/main/src/platform/linux/platform.cpp):
  date-sensitive conversion between UTC and local wall time.
- [files.cpp](../src/files.cpp)'s `scan_media_file` and `file_scan_result::parse_media_properties`: an
  audio or video file as libfiles' media scan reads it, in Diffractor's own terms; and
  `file_scan_result::parse_metadata_ffmpeg_kv`: the tags FFmpeg reads of a file libfiles cannot describe
  whole, which [av_format.cpp](../src/av_format.cpp) describes.
- [model_property.cpp](../src/model_property.cpp): shared property identity and formatting, and
  `prop::item_metadata::read_colour`, a picture's colour profile and HDR, from the profile name
  [files.cpp](../src/files.cpp)'s `icc_profile_of` reads and the coded colour description.
- [files.cpp](../src/files.cpp)'s `files::scan_c64_media`: a Commodore image's listing as its properties.
- [model_index.cpp](../src/model_index.cpp): films, episodes and shows read from file and folder names
  (`scan_info_from_title`, `derive_name_values`), and what each collection folder holds
  (`index_state::library_kind_of`); [files.cpp](../src/files.cpp): media managers' `.nfo` files
  (`read_video_nfo`, `apply_nfo`) and the art kept beside music and films (`load_folder_art`).
- [model_db_pack.h](../src/model_db_pack.h) and [model_db.cpp](../src/model_db.cpp): cached metadata
  serialization.
- [files.cpp](../src/files.cpp): format dispatch and scan publication. Every still - JPEG, PNG, WebP,
  GIF, TIFF, PSD, BMP, HEIF and AVIF, and JPEG XL - is scanned by
  [libfiles](https://github.com/ZacWalk/libfiles), whose result `scan_still_libfiles` maps onto a
  `file_scan_result`; [files_heif.cpp](../src/files_heif.cpp) decodes the HEIF thumbnail item a scan
  wants, and libfiles reads a JPEG XL's compressed boxes with its own Brotli decoder.