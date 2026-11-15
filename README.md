# Diffractor
[![CI](https://github.com/ZacWalk/diffractor/actions/workflows/windows.yml/badge.svg)](https://github.com/ZacWalk/diffractor/actions/workflows/windows.yml)

Free, high-performance photo, video and audio organizer for Windows. Optimized for speed and local
file control—no cloud storage or subscriptions required. A Linux build runs but is not distributed;
see [docs/linux.md](docs/linux.md).

![Diffractor screenshot](screenshot.webp)

## Features

| Category | Capabilities |
|----------|-------------|
| **Viewing** | Unified photo/video/audio playback; native support for most formats including RAW; Commodore 64 SID tunes, with a voice-by-voice SID monitor; C64 disks, tapes, cartridges and programs run in a built-in Commodore 64 emulator |
| **Zoom** | One scale-and-center model across mouse, keyboard, wheel, and touch; hold to inspect, an overview navigator, and linked side-by-side magnification |
| **Search** | Instant metadata-based search (tags, location, camera data) via local indexing |
| **Places** | Photos and videos are placed from GPS alone, searchable by place, city, state, country, or distance, with map and visit-derived grouping |
| **Metadata** | Read/write XMP, IPTC, EXIF, ID3; add tags, ratings, location |
| **Organization** | Side-by-side comparison, and duplicate detection ("Presence") that recognizes a re-encoded, resized, or rotated copy as well as an identical one |
| **Related items** | Finds possible copies, the same album, the same series, and the closest capture times and places, each as its own group |
| **File operations** | Rename, Import, and Sync preview every file before they run, and state how each name collision will be resolved |
| **Sync** | Bidirectional sync to NAS/network drives for backup and collaboration |
| **Editing** | Resize, rotate, crop, perspective, temperature and tint, color adjustment, and automatic color, straighten, and document correction |
| **Languages** | English plus 13 translation catalogs |

## Building

```bash
git clone https://github.com/ZacWalk/diffractor.git
```

The build uses CMake and requires Visual Studio with the C++ workload, Ninja, Python 3 and
PowerShell 7. CMake fetches the dependencies at the versions `cmake/dd-dependencies.json` pins,
upstream [FFmpeg](https://github.com/FFmpeg/FFmpeg) among them.

Everything goes through the `dd` gateway, which finds Visual Studio, configures with Ninja and puts the binary in `exe/`:

```powershell
python tools/dd.py build                                 # Debug x64: exe\diffractor64-d.exe
python tools/dd.py build --config Release                # Release x64: exe\diffractor64.exe
.\dd.ps1 test                                            # build, lint, test, check translations
.\dd.ps1 test --label headless                           # the same complete gate, no visible UI
python tools/dd.py build --config Release --arch x86     # the 32-bit desktop binary
python tools/dd.py build --winstore                      # the Store binary
.\dd.ps1 linux                                           # build the Linux binary in WSL and run it
.\dd.ps1 vs                                              # debug the Debug build in Visual Studio
```

Release packaging also requires NSIS, the Windows SDK, and a code-signing certificate; `dd.ps1`
fetches the 7-Zip it packs with. Run `.\dd.ps1` for the complete command list.
Diffractor's own driver uses the `headless` label for its complete windowless
gate (Release on Windows), rather than CTest presets. Its console reports each
case and the real pass/fail totals; failed or empty label selections fail
nonzero. For a filtered run after building, use
`& .\exe\diffractor64-d.exe /test:*layout* 2>&1 | Out-Host; $LASTEXITCODE`:
the pipe makes PowerShell wait for the GUI-subsystem executable.

Product behavior is documented in [docs/design.md](docs/design.md). Architecture is
documented in [docs/implementation.md](docs/implementation.md), whose links lead to
the specialized subject documents. Contributor routing and repository rules are in
[AGENTS.md](AGENTS.md).

### Icon artwork

`python tools\generate_icon.py` uses the interlocking square geometry from
`tools\logo.html` with a saturated palette for desktop and installer ICOs, logo PNGs and Linux artwork.
`python tools\generate_icon.py --check` checks the committed bytes without writing.
For another size, use `--output tmp\icon-128.png --size 128`.
`python tools\generate_store_assets.py --output exe\Assets` produces Store artwork
from the same vectors; it also accepts `--check`. Both use Python 3.9+'s standard
library and the self-contained `tools\iconlib.py`, with no fonts or Pillow required.

### Recording the window

A Debug build on Windows can record its window, menus and pointer included, to an MP4 at 30 fps for
tutorial videos. `-record-size` sets the window's size first, and `-record-seconds` stops and closes it
after that long; without it the recording ends when the window closes. Keep the window on screen and
uncovered, and the screen unlocked, while it records.

```powershell
& .\exe\diffractor64-d.exe -record:C:\videos\take.mp4 -record-size:1920x1080 -record-seconds:30 2>&1 | Out-Host
```

`-tutorial:<script.dft>` acts out a tutorial script instead: it sets a scene, starts the recording, plays
each beat of narrated steps with a drawn pointer, writes when each beat starts beside the movie, and
closes the window. Without `-record` it is a rehearsal. `-collection:<folder>` makes one folder the whole
collection, and `-profile:<folder>` keeps settings and the index there, so neither touches your own;
`-collection` alone keeps them in `<folder>-profile`. The script grammar is in
[app_tutorial.h](src/app_tutorial.h).

## Rendering Movie projects from the command line

A Movie project is a `.dfv` file: plain text, one item per line in the order they play, which a person
or an agent can write. `t` is a screen of text on black for so many seconds; `c` is a clip, from and
to in seconds, with an optional caption and an optional `!` sound file played in place of its own sound;
`s` is a clip marked to be stabilized.

```text
t 4 'Hiking to Everest'
c 12.5-31 2019/kathmandu.mp4 'Kathmandu'
s - "2019/day 3/climb.mp4"
c -6 2019/lukla.jpg !narration/lukla.wav
t 3 'The End'
```

The command line checks and renders projects without opening a window. The format is in
[docs/movie.md](docs/movie.md#6-project-files), and the switches in
[docs/movie.md](docs/movie.md#9-command-line).

| Switch | Effect |
|--------|--------|
| `-movie-check:<project or folder>` | Reports what render would find in each `.dfv`, including any line it cannot read. Writes nothing |
| `-movie-render:<project or folder>` | Writes an `.mp4` for each `.dfv` project whose lines all read and whose sources are all usable |
| `-movie-out:<folder>` | Sets where rendered movies go. By default each goes beside its project |
| `-movie-overwrite` | Replaces a movie that already exists instead of skipping it |
| `-movie-skip-missing` | Renders without the clips whose source cannot be used |

```powershell
.\exe\diffractor64.exe -movie-check:D:\Projects | Out-Host
.\exe\diffractor64.exe -movie-render:D:\Projects -movie-out:D:\Movies | Out-Host
```

Paths must be full paths. Quote the whole switch when a path contains a space, as in
`"-movie-render:D:\My Projects\trip.dfv"`. Piping into `Out-Host` makes PowerShell wait and show the
report as it is written.

## Contributing

Contributions are coordinated through [GitHub issues](https://github.com/ZacWalk/diffractor/issues).
Use [Poedit](https://poedit.net/) for catalogs under `exe/languages`, and run
`.\dd.ps1 test` before submitting a change.

## License

Diffractor's source code is licensed under the [Mozilla Public License 2.0](LICENSE), as are
[libdesktop](https://github.com/ZacWalk/libdesktop) and [libfiles](https://github.com/ZacWalk/libfiles),
which it is built on. Anyone distributing a binary must tell its recipients where to get that source
code, including any changes made to it.

A released binary also includes libraries under the LGPL: libheif and libde265 (LGPL-3.0-or-later),
FFmpeg (LGPL-2.1-or-later, configured with `CONFIG_GPL 0` so that no GPL-only component is included),
and the code libfiles ports from libexif and libiptcdata (LGPL-2.1-or-later). Before distributing a
binary:

- Ship their licence texts beside it, as every release package does from [licences](licences): the
  LGPL-3.0, the GPL-3.0 it incorporates, and the LGPL-2.1.
- Let its users relink it with modified versions of those libraries (LGPL-2.1 section 6, LGPL-3.0
  section 4): provide the corresponding source of their exact revisions, including changes, and the
  application source and build scripts or object files needed to rebuild it. A link to the latest
  source alone is not a relinking package.
- Permit modification for the recipient's own use and reverse engineering to debug those
  modifications.

The other fetched libraries keep their own licences and notices; the
[dependency inventory](docs/third-party.md#dependency-inventory) lists them all.
