# Third-Party Dependencies

This document owns the dependency inventory, upstream sources, licensing, vendoring policy, and the integration
constraints an upgrade must preserve. The pins in [dd-dependencies.json](../cmake/dd-dependencies.json) and the
revisions the libfiles, libdesktop, lib64 and libmug modules name are authoritative, and CMake owns the exact
compiled source lists. Wrapper behavior belongs to the corresponding subject document.

## Licensing

Diffractor's own licence is stated in the [README](../README.md#license): its code, libdesktop and libfiles are
MPL-2.0, apart from the files libfiles ports from libexif and libiptcdata, which keep LGPL-2.1-or-later; lib64 and
libmug are MIT. libheif and libde265 are LGPL-3.0-or-later and FFmpeg is LGPL-2.1-or-later, all statically linked,
so a released binary carries their LGPL obligations. [licences](../licences) holds the LGPL-3.0, the GPL-3.0 it
incorporates and the LGPL-2.1, beside the licences of libaom and of the libfacedetection and SFace weights libmug
compiles in, and every release package - the installer, the portable zip and the Store package - carries them
beside the executable with Diffractor's own licence, as each requires of a copy. They stay out of the repository
root and out of a top-level `LICENSES` folder, where GitHub would read them as Diffractor's own licence.

FFmpeg is configured without GPL, version-3, or LGPL-v3-only components, so it remains usable under LGPL 2.1.
Enabling a GPL component changes the outbound licence of the combined binary and is not an ordinary build option.

A dependency or model with non-commercial terms, no redistribution grant, or incompatible additional restrictions
cannot ship. Every dependency keeps the licence and notices its exact upstream version requires; the licence file
in its pinned archive is the authority for those terms.

## Vendoring

Nothing is vendored. The sibling libraries are fetched at the commits their `cmake/Diffractor*.cmake` modules name,
and every other dependency as an archive a manifest pins by SHA-256 - skcms, which makes none, by commit: Diffractor's
own [dd-dependencies.json](../cmake/dd-dependencies.json), an upstream FFmpeg commit among them, or the
`cmake/dd-dependencies.json` of libfiles, libdesktop or libmug for a library one of them fetches. Assets live in
their consuming tree. Owned CMake modules under `cmake/vendored/` select each library's sources, definitions and
compiler policy, and keep beside them the configuration headers a library's own build would have generated.

Fetched source is upstream material, never patched where it lands, and Diffractor keeps no fork of a dependency.
Keep Diffractor changes in wrappers and CMake modules; a configuration header kept in `cmake/vendored/` is part of
its library's upgrade checklist.

A library that offers one may instead come from a system package, for bringing a new platform up
(`DIFFRACTOR_SYSTEM_<NAME>`, or `DIFFRACTOR_PREFER_SYSTEM` for all of them; both off by default). Nothing ships
that way, and FFmpeg is never taken from the system: a distribution's build typically carries GPL components,
which would change the outbound licence.

## Dependency Inventory

Update this table with each pin. A snapshot is an exact upstream commit, not a moving `main` branch or the API
version left in its headers. *Fetched* is pinned in [dd-dependencies.json](../cmake/dd-dependencies.json) and built
by a module in `cmake/vendored/`; *libfiles*, *libdesktop* or *libmug* is pinned in that library's own
`cmake/dd-dependencies.json` and built by its recipe; *sibling* is a commit in a `cmake/Diffractor*.cmake` module,
and its `../<name>` checkout is built instead when present. libfiles builds each of its libraries as
`libfiles::<name>`, which [CMakeLists.txt](../CMakeLists.txt) also names `diffractor::<name>` for the codecs and
tests that link it. Archives are pinned by SHA-256.

| Library | Imported release or upstream snapshot | Location | Type | Upstream, and what it is here |
| --- | --- | --- | --- | --- |
| Brotli | 1.2.1 | libfiles | Release archive | [google/brotli releases](https://github.com/google/brotli/releases): its C decoder and encoder; libfiles' bounded decoder reads JPEG XL's compressed metadata boxes with it, and libjxl links it |
| bzip2 | 1.0.8 | Fetched | Release archive | [Sourceware](https://sourceware.org/bzip2/downloads.html), built unchanged by [bzip2.cmake](../cmake/vendored/bzip2.cmake) |
| dav1d | 1.5.4 | libfiles | Release archive | [VideoLAN](https://code.videolan.org/videolan/dav1d), with libfiles' configuration and bitdepth-template wrappers ([dav1d](#dav1d)); libheif and FFmpeg's software AV1 decoder link it |
| Adobe DNG SDK | 1.7.1 build 2802 | libfiles | Release archive | [Adobe source archive](https://download.adobe.com/pub/adobe/dng/dng_sdk_1_7_1_2802_20261110.zip): `dng_sdk/source/` - every source but the SDK's XMP backend, which libfiles' [dng_xmp_sdk.cpp](https://github.com/ZacWalk/libfiles/blob/main/src/dng_xmp_sdk.cpp) replaces - with the `RawEnvironment.h` kept in libfiles' `cmake/dng/`; LibRaw hands it DNGs |
| FFmpeg | `d35e13b9e05cc20f48373f88e399c8ef0c16fc61`, upstream `master` of 2026-11-16 | Fetched | Upstream commit archive | [FFmpeg/FFmpeg](https://github.com/FFmpeg/FFmpeg). [FFmpeg.cmake](../cmake/vendored/FFmpeg.cmake) drives its configure and make on Linux, and [ffmpeg_msvc.cmake](../cmake/vendored/ffmpeg_msvc.cmake) builds it on Windows with the configuration its configure generated, kept in `cmake/vendored/ffmpeg-windows/` ([FFmpeg](#ffmpeg)) |
| Highway | 1.4.0 | libfiles | Release archive | [google/highway releases](https://github.com/google/highway/releases), which libjxl and libmug use |
| Hunspell | 1.7.6 | Fetched | Release archive | [Hunspell releases](https://github.com/hunspell/hunspell/releases); [hunspell.cmake](../cmake/vendored/hunspell.cmake) builds its `src/hunspell/` unchanged, the version and visibility headers included |
| lib64 | `DIFFRACTOR_LIB64_REVISION` in [DiffractorLib64.cmake](../cmake/DiffractorLib64.cmake) | Sibling | Commit, MIT | [ZacWalk/lib64](https://github.com/ZacWalk/lib64), its emulator core only. Its Commodore 64 plays SID tunes, which need no ROM, and shows the chip's registers for [av_sid.cpp](../src/av_sid.cpp), checks a tune's header for [files.cpp](../src/files.cpp), and runs a disk's, tape's, cartridge's or program file's program on the KERNAL, BASIC and character ROMs it carries for [av_c64.cpp](../src/av_c64.cpp) |
| libaom | 3.15.1 | libfiles | Release archive | [AOMedia releases](https://storage.googleapis.com/aom-releases/), signed with AOMedia's release key; BSD-2-Clause with the Alliance for Open Media Patent License 1.0, whose texts every release package carries from [licences](../licences). Its encoder alone, with the configuration its own CMake generated, in libfiles' `cmake/aom/` ([libaom](#libaom)): libheif's AV1 encoder, which libfiles writes AVIF with |
| libarchive | 3.8.11 | Fetched | Release archive | [libarchive releases](https://github.com/libarchive/libarchive/releases); [archive.cmake](../cmake/vendored/archive.cmake) builds it unchanged, its CRC32 and umask helpers included, with the configuration its configure produced for Windows and for Linux kept in `cmake/vendored/archive-windows/` and `archive-linux/` |
| libde265 | 1.1.3 | libfiles | Release archive | [strukturag/libde265 releases](https://github.com/strukturag/libde265/releases), with the `config.h` and `de265-version.h` its own build generates: libheif's HEVC decoder where FFmpeg's is not named |
| libdesktop | `DIFFRACTOR_LIBDESKTOP_REVISION` in [DiffractorLibdesktop.cmake](../cmake/DiffractorLibdesktop.cmake) | Sibling | Commit, MPL-2.0 | [ZacWalk/libdesktop](https://github.com/ZacWalk/libdesktop), its four static layers: Diffractor links `libdesktop::ui`, which brings `libdesktop::core`, `libdesktop::platform` and `libdesktop::render`; [AGENTS.md](../AGENTS.md#source-directory-map) says what each supplies |
| libfacedetection | `acf7b254121927e7dced30e233a2e03119f28ea2`, upstream `master` of 2026-06-28 | libmug | Commit archive | [ShiqiYu/libfacedetection](https://github.com/ShiqiYu/libfacedetection), BSD-3-Clause, whose text every release package carries from [licences](../licences). libmug compiles in its YuNet weights, converted, and runs the network on its own kernels; nothing else of it is built. Its sample photographs are the face tests' ([Faces](#faces)) |
| libfiles | `DIFFRACTOR_LIBFILES_REVISION` in [DiffractorLibfiles.cmake](../cmake/DiffractorLibfiles.cmake) | Sibling | Commit, MPL-2.0; its files ported from libexif and libiptcdata are LGPL-2.1-or-later | [ZacWalk/libfiles](https://github.com/ZacWalk/libfiles), shared with ImageWalker: format identification, still-image and audio/video scans, reconciled properties, XMP/EXIF/IPTC, the ICC profile listing, metadata updates, still-image decoding and encoding, camera raw loading, and the Commodore 64 listing. Diffractor supplies streams and pixel storage, and builds it without its GIF and TIFF documents (`LIBFILES_GIF`, `LIBFILES_TIFF`), so giflib and libtiff are not built; libheif is called here only to register its FFmpeg decoder plugin, and libjxl only by the DNG SDK |
| libheif | 1.23.7 | libfiles | Release archive | [strukturag/libheif releases](https://github.com/strukturag/libheif/releases): its library with the JPEG, libde265, dav1d and uncompressed decoders and the libaom and uncompressed encoders, for libfiles' HEIF decoder and AVIF encoder. Its FFmpeg decoder plugin is built here from that tree by [DiffractorHeif.cmake](../cmake/DiffractorHeif.cmake) and registered by [files_heif.cpp](../src/files_heif.cpp), which names it to libfiles for HEVC images |
| libjpeg-turbo | 3.2.1 | libfiles | Release archive | [libjpeg-turbo releases](https://github.com/libjpeg-turbo/libjpeg-turbo/releases): libfiles decodes, encodes and losslessly turns JPEGs with it, and the codecs that read JPEG data and the tests link it |
| libjxl | 0.13.0 | libfiles | Release archive | [libjxl releases](https://github.com/libjxl/libjxl/releases): its decoder, encoder, colour management and thread runners, with the version and export headers its own build generates, optimised like the rest; libfiles decodes and encodes JPEG XL with it, and the DNG SDK links it |
| XZ / liblzma | 5.8.4 | Fetched | Release archive | [XZ releases](https://github.com/tukaani-project/xz/releases); [lzma.cmake](../cmake/vendored/lzma.cmake) builds `src/liblzma/` and `src/common/` unchanged, not the command-line tools, with the `config.h` kept in `cmake/vendored/lzma/` |
| libmug | `DIFFRACTOR_LIBMUG_REVISION` in [DiffractorLibmug.cmake](../cmake/DiffractorLibmug.cmake) | Sibling | Commit, MIT | [ZacWalk/libmug](https://github.com/ZacWalk/libmug): finds faces and makes each a vector for [faces](faces.md), YuNet and SFace on its own SIMD kernels over libfiles' Highway (`LIBMUG_HIGHWAY_TARGET`), both networks' weights compiled in ([Faces](#faces)) |
| libopenmpt | 0.8.10 | Fetched | Release archive | [libopenmpt downloads](https://lib.openmpt.org/libopenmpt/download/), the `.autotools` source release; [openmpt.cmake](../cmake/vendored/openmpt.cmake) builds it unchanged, with the release's own `svn_version.h` ([libopenmpt](#libopenmpt)) |
| libpng | 1.6.59 | libfiles | Release archive | [libpng](https://www.libpng.org/pub/png/libpng.html), [upstream tags](https://github.com/pnggroup/libpng/tags): built in C alone, with the `pnglibconf.h` kept in libfiles' `cmake/`; libfiles decodes and encodes PNGs with it, and the tests link it |
| LibRaw | 0.22.3 | libfiles | Release archive | [LibRaw releases](https://github.com/LibRaw/LibRaw/releases): its library, with the DNG SDK and libjpeg. libfiles loads raw files with it - their preview, or their raw data developed - and scans them: their metadata, the info pane's listing and a thumbnail. Diffractor links it for libfiles without calling it |
| libwebp | 1.6.0 | libfiles | Release archive | [libwebp tags](https://github.com/webmproject/libwebp/tags): its decoder, encoder, sharpyuv, mux and demux as one library; libfiles decodes still WebPs and encodes WebPs with it, and the tests link it |
| minizip-ng | 4.2.2 | Fetched | Tag archive | [minizip-ng releases](https://github.com/zlib-ng/minizip-ng/releases); [minizip.cmake](../cmake/vendored/minizip.cmake) builds it unchanged, the `compat/` API included, with the `mz_config.h` kept in `cmake/vendored/minizip/` |
| RapidJSON | `297637bc0a0f00dc504dbc1a47ebdaa577f8ae19` | libdesktop | Commit archive | [Tencent/rapidjson](https://github.com/Tencent/rapidjson): headers only, which libdesktop gives `libdesktop::core`'s consumers; the agent tools and the Movie report read and write JSON with them ([app_mcp_json.h](../src/app_mcp_json.h), [app_movie_batch.cpp](../src/app_movie_batch.cpp)), and nothing of it is built |
| SDL3 | 3.4.20 | libdesktop, on Linux | Release archive | [libsdl-org/SDL releases](https://github.com/libsdl-org/SDL/releases), linked statically into libdesktop's Linux platform and render backends; the Windows build does not use it |
| SFace | OpenCV Zoo `ba91a3b91d00d76e86540d4013f944bd6b514e39`, `face_recognition_sface_2021dec.onnx` | libmug | Model file | [opencv/opencv_zoo](https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface), Apache-2.0, whose text every release package carries from [licences](../licences). libmug converts it as the build runs into the quantized weights it compiles in ([Faces](#faces)) |
| skcms | `074a8d439d75df5bff2746458306f7760ee74dd8` | libfiles | Git commit | [Skia upstream](https://skia.googlesource.com/skcms/). It makes no releases, and Skia's generated archives differ from one download to the next, so libfiles clones the commit; its baseline, Haswell and Skylake units serve libjxl's colour management |
| SQLite | 3.54.1 | Fetched | Amalgamation archive | [sqlite.org](https://www.sqlite.org/download.html); [sqlite.cmake](../cmake/vendored/sqlite.cmake) builds the amalgamation unchanged, with Diffractor's compile-time options |
| zlib-ng | 2.3.3, zlib-compatible API | libfiles | Release archive | [zlib-ng releases](https://github.com/zlib-ng/zlib-ng/releases), not stock zlib, with the headers its own build generates; libpng, libarchive, minizip, the DNG SDK and FFmpeg link it |

Version traps matter during an audit: RapidJSON's `1.1.0` does not identify a post-release snapshot,
and zlib-ng's `1.3.1.zlib-ng` describes its compatibility API. Use the SDK build number for DNG, the
public `hunversion.h` for Hunspell, and `ZLIBNG_VERSION` in the generated `zlib.h` - libfiles'
`cmake/zlib-ng/zlib.h` - for zlib-ng rather than stale package strings or input templates.

### Build Tools

| Tool | Version | Location | Upstream / retained notice |
| --- | --- | --- | --- |
| NASM | 3.02 | Fetched while configuring on Windows when none is on the path | [NASM Windows archives](https://www.nasm.us/pub/nasm/releasebuilds/3.02/), pinned in [dd-dependencies.json](../cmake/dd-dependencies.json) at the release libfiles pins; its `LICENSE` is in the archive |
| 7-Zip Extra | 26.06 | Fetched by `dd.ps1` into `tmp/tools/7zip/` | [7-Zip release](https://github.com/ip7z/7zip/releases/tag/26.06): `7zr.exe` unpacks `7z2606-extra.7z`, both pinned in [dd-dependencies.json](../cmake/dd-dependencies.json); its `License.txt` is in the archive |
| SymStore | Installed Windows SDK | `Windows Kits\10\Debuggers\x64\symstore.exe` | Machine-installed SDK, or a per-user SDK administrative image as described in [crash.md](crash.md#3-symbols); packaging checks it before changing release state |

`dd.ps1` packs with the release's x86 `7za.exe` and its two DLLs; SignTool and SymStore come from the installed
Windows SDK. These are build and packaging tools, not media decoders. The Visual Studio, CMake, Ninja, Windows SDK
and NSIS prerequisites are the [README](../README.md#building)'s.

## Upgrade Procedure

1. Identify the exact upstream release or commit and verify its licence.
2. Pin it: the archive's URL and SHA-256 in [dd-dependencies.json](../cmake/dd-dependencies.json), or in the pins
   of libfiles, libdesktop or libmug for a library one of them fetches; a sibling library's new commit goes in its
   `cmake/Diffractor*.cmake` module once pushed there. Compare the new release's source inventory with the old
   one's.
3. Reapply the integration constraints below - a configuration header kept in `cmake/vendored/` is brought up to
   the release - update the owning CMake module's source list, and update the inventory. Record snapshot commit IDs
   and DNG build numbers, not just a branch or API version.
4. Configure from a clean build directory so removed sources cannot survive as stale objects. In an existing tree
   the pin manifest is a configure dependency, so a pin change refetches the archive, and extraction uses current
   timestamps so existing objects rebuild; FFmpeg's staged source depends on FFmpeg's pin alone, so an incremental
   Linux build copies a new snapshot before configuring it, and another library's pin change does not rebuild
   FFmpeg.
5. Build Debug and Release x64 plus Release Win32 where the dependency has architecture-specific code, then run
   `.\dd.ps1 test` and its focused wrapper tests.
6. Before packaging, configure a clean build directory in an isolated checkout, or explicitly set
   `FETCHCONTENT_SOURCE_DIR_LIBDESKTOP`, `FETCHCONTENT_SOURCE_DIR_LIBFILES`, `FETCHCONTENT_SOURCE_DIR_LIB64` and
   `FETCHCONTENT_SOURCE_DIR_LIBMUG` to empty cache values. Confirm the configure output names the expected pinned
   revisions, then build and test the release. A build against sibling worktrees cannot detect an API the pinned
   commits are missing.

Build only what the application needs from a fetched archive: not upstream tests, benchmarks, examples or build
systems. CMake source lists are explicit; a file in a fetched archive is not part of Diffractor until a module
names it.

## FFmpeg

FFmpeg is upstream [FFmpeg](https://github.com/FFmpeg/FFmpeg), fetched at the commit
[dd-dependencies.json](../cmake/dd-dependencies.json) pins and built unpatched: an upgrade is a new pin and a
regenerated Windows configuration. Diffractor uses it to decode - playback, thumbnails, frame extraction, audio for
the waveform and the movie encoder's sources - to describe the streams of a file libfiles' media scan cannot
describe whole, and to copy a video's or a sound file's packets into a new file of its own format, or an MP4, for
Edit's lossless edits. Every other audio and video read - the container's streams, duration, tags, XMP packet and
cover art - is libfiles' ([metadata](metadata.md)).

Its configuration controls the available demuxers, muxers, decoders, parsers and hardware acceleration. It builds
no end-user programs and enables no network protocols. It is configured for decode speed rather than size, which
also keeps codec profile names; it decodes software AV1 through libdav1d, built against libfiles' dav1d; D3D11VA,
on Windows, is its only hardware acceleration; its only muxers write the formats Edit copies a file into; and it
has no encoders or devices. It is configured LGPL 2.1: no GPL, version-3 or nonfree component.

[av_format.cpp](../src/av_format.cpp) owns what FFmpeg's answers become: codec names in Diffractor's short form, a
pixel format as chroma and depth, the coded sample type, and iTunes' `rtng` advisory kept out of the rating; its
metadata probe decodes only H.264 key frames, which is where the decoder applies the SPS. Audio is converted only
through a resampler `swr_init` accepted, which refuses invalid formats and channel layouts. Architecture dispatch
must use real compiled kernels or exclude foreign-architecture sources; unresolved SIMD symbols must never be
answered by no-op stubs.

The Windows configuration is architecture-specific. Linux uses FFmpeg's own configure and make rather than
restating its codec inventory in application CMake; there FFmpeg's reported version label can name the surrounding
Diffractor checkout rather than the commit archive, so the pin manifest, not that label, identifies the upstream
snapshot.

### FFmpeg Configuration And Upgrade Checklist

- Configure an LF-only source export in a separate build directory. For a Windows cross-configure,
  use MinGW with the matching x86 or x64 target, runtime CPU detection, external x86 assembly,
  no inline assembly, Windows threads, and zlib. Disable programs, documentation, network,
  encoders, muxers, devices and libavdevice, DXVA2, filters and general protocols; retain the
  `file` protocol, and enable again the muxers Edit writes, the `--enable-muxer=` list in
  [FFmpeg.cmake](../cmake/vendored/FFmpeg.cmake). Enable libdav1d and do not pass `--enable-small`,
  which strips profile names and trades decode speed for size. Do not enable GPL, version-3 or
  nonfree components. The MinGW toolchain has no dav1d, so give configure a `dav1d.pc` and a stub
  library answering its `dav1d_version` probe, through a cross-prefixed `pkg-config` so the recorded
  configure line stays the one above.
- Keep the `config.h` and `config.asm` dispatchers in `cmake/vendored/ffmpeg-windows`. Regenerate
  `config-x64.h`, `config-x86.h` and their assembly equivalents independently. MinGW compiler and libc
  probes are not MSVC probes: preserve the MSVC capability overrides, disable GNU inline assembly,
  and keep `HAVE_ALIGNED_STACK` disabled on Win32. MSVC does not supply `unistd.h`; do not
  accidentally select the application compatibility header instead of FFmpeg's native Windows I/O
  support. Recover the overrides rather than recalling them: configure the old pin the same way,
  and what separates its raw output from the vendored copy is the set to carry, by name, onto the
  new commit's. Judge each switch the new commit adds.
- Reconcile the already-linked bzip2, liblzma and libopenmpt backends when the cross-configure
  cannot detect them. libopenmpt needs both its component switch and its entry in the generated
  demuxer list. A switch without a compiled implementation is not support.
- Generate the shared component headers and codec, parser, bitstream-filter, demuxer, muxer and
  protocol lists into `cmake/vendored/ffmpeg-windows`, at the paths FFmpeg includes them by. Verify
  the shared files agree between architectures. Stamp `libavutil/ffversion.h` with the upstream
  revision. Upstream commits swscale's generated assembly macros (`libswscale/x86/uops_macros.asm`),
  so nothing else is generated.
- Derive source changes from complete `make -Bn` dry runs for both architectures, not an
  incremental `make -n`. Update [ffmpeg_msvc.cmake](../cmake/vendored/ffmpeg_msvc.cmake), remove
  deleted source names, and retain assembly include paths, unique object names and Win32
  exclusions. The atomics compatibility layer needs both `compat/atomics` and
  `compat/atomics/win32` on the MSVC include path.
- The monolithic Windows archive compiles a shared implementation only once, not also through the forwarding
  sources upstream's separate libraries use.
- Compare the Windows/Linux component sets with `tools/compare_ffmpeg_config.py`. Explain any
  intentional platform difference in `cmake/vendored/ffmpeg-config-divergence.txt`; do not simply
  accept a changed baseline. A working Windows build does not verify the Linux configuration.
- Prove decoding and the metadata probe through the application wrapper tests. Run the metadata,
  video, audio and format tests, then the complete suite. Do not replace a missing assembly
  implementation with an empty symbol or drop a decoder to make the link pass.

## Image And Archive Libraries

Image decoders, encoders, compression libraries, SQLite, and archive readers are built through their
owned CMake modules. Keep generated configuration multi-architecture where one source tree serves
x86 and x64. Preserve application wrappers as the sole boundary for errors, allocation limits,
pixel formats, and cancellation; vendor callbacks must not escape those contracts.

An upgrade that changes a file format's metadata, orientation, color, alpha, or malformed-input
behavior requires the corresponding format tests, not only a compile. A libheif upgrade is held by
the whole HEIF sweep, "Should decode HEIFs as load_heif did" in [test_files.cpp](../src/test_files.cpp):
every complete file keeps its outcome, and a damaged input's outcome may change only as the sweep's
comment records it.

### Source Subsets And Generated Files

| Dependency | Integration that an import must preserve |
| --- | --- |
| libfiles, libdesktop, lib64, libmug | A new commit pin in their `cmake/Diffractor*.cmake` module, pushed there first. test_metadata's records of libfiles' reading of every fixture packet, whole and cut short, and of every packet it writes must hold, as must the metadata behaviors [metadata.md](metadata.md) describes; [file-io.md](file-io.md) owns the write path and rollback boundary. A successful build proves none of them. |
| libpng / zlib-ng | Upgraded in libfiles: a new archive pin, with the headers their own builds generate - libpng's `pnglibconf.h`, zlib-ng's `zlib.h`, `zconf.h` and `zlib_name_mangling.h`, in libfiles' `cmake/` - brought up to the release. Use the upstream source lists to identify added or removed units; zlib-ng's per-instruction-set sources need their compiler flags. libpng is built in C alone; turning on its SIMD filters is a change to measure, not part of an upgrade. |
| libheif / libde265 | Upgraded in libfiles: a new archive pin, with the headers their own builds generate - libheif's `heif_version.h`, libde265's `config.h` and `de265-version.h`, in libfiles' `cmake/` - brought up to the release. Keep static linkage, the selected decoder backends, the libaom encoder and libde265's platform configuration; do not enable additional encoder/plugin dependencies implicitly - an HEVC encoder, x265, is GPL. Keep libheif's FFmpeg decoder plugin, which [DiffractorHeif.cmake](../cmake/DiffractorHeif.cmake) builds from libfiles' libheif: [files_heif.cpp](../src/files_heif.cpp) registers it and names it for HEVC images, which it decodes faster than libde265 and, unlike libde265's 32-bit build, bit-exactly. |
| libjpeg-turbo | Upgraded in libfiles: a new archive pin, with the configuration headers its `cmake/libjpeg-turbo/` carries for MSVC and for GCC and Clang brought up to the release. Use the upstream source lists to identify added or removed units, and preserve the high-bit-depth libraries. |
| libjxl / Highway / Brotli | Upgraded in libfiles: a new archive pin, with the version and export headers libjxl's own build generates, in libfiles' `cmake/jxl/include/`, brought up to the release. Use the upstream source lists to identify added or removed units, and preserve libjxl's Highway, Brotli and skcms dependencies. An upgrade runs the JPEG XL tests in Release, optimised: "Should scan jxl metadata" is where an MSVC miscompile that decodes a JPEG XL to an empty image would show. |
| liblzma | A new archive pin. Keep `cmake/vendored/lzma/config.h` and the Linux POSIX-thread override, and build the library and common subset, not the differently licensed command-line scripts and tools. |
| libarchive | A new archive pin. Keep `cmake/vendored/archive-windows/config_windows.h` and `archive-linux/config_linux.h`, which `PLATFORM_CONFIG_H` names, and bring them up to the release's configure output for each platform. Retain the CRC32 and umask sources, the Windows `ARCHIVE_CRYPTO_WIN` switch as well as its per-digest switches, and Linux's `HAVE_WAITPID`; otherwise encryption or external-filter process handling can silently disappear. Windows leaves `HAVE_FCNTL` undefined, as upstream's Windows configuration does: it has no `fcntl()`, and 3.8.11 calls one. Its XAR reader is built without an XML parser, so nothing needs Expat. |
| minizip-ng | A new archive pin. Keep `cmake/vendored/minizip/mz_config.h`, the configuration Windows and Linux both build with, and bring it up to the release's `mz_config.h.cmakein`. Its zlib-style compatibility API lives in `compat/`; retain it and the platform-specific crypto selection. |
| Hunspell | A new archive pin. Keep the static-library configuration. The source list includes `hunspelltrace.cxx`; dropping it leaves the trace hooks unresolved. |
| LibRaw / DNG SDK | Upgraded in libfiles: a new archive pin each, with the SDK's `RawEnvironment.h`, in libfiles' `cmake/dng/`, kept. Preserve the LibRaw wrapper boundary. Build only the SDK's `dng_sdk/source/`, not Adobe's bundled copies of libraries libfiles owns. The SDK's `dng_xmp_sdk.cpp` is not built: libfiles' [dng_xmp_sdk.cpp](https://github.com/ZacWalk/libfiles/blob/main/src/dng_xmp_sdk.cpp) implements the SDK's `dng_xmp_sdk.h` over libfiles' XMP model in its place, so an upgrade that changes `dng_xmp_sdk.h` or `dng_xmp_sdk.cpp` must carry the change there - the namespaces `InitializeSDK` registers, which faults each call reports, swallows or throws, and the toolkit utilities it reproduces. The SDK is built with `qDNGXMPFiles=0`, and document-history support remains disabled, because that backend has neither XMPFiles nor `SXMPDocOps`. |
| skcms | Upgraded in libfiles: a new commit pin. Keep the baseline, Haswell and Skylake transform units and their compiler-specific instruction-set flags. Validate ICC transforms, not only compilation. |
| RapidJSON | Upgraded in libdesktop: a new archive pin. Its upstream `bin/jsonchecker/` has a different, restrictive license, and nothing of it is built or shipped. |

### dav1d

Upgraded in libfiles, whose `cmake/dav1d/` holds its configuration. Keep the four bitdepth-template
wrappers that compile the upstream template sources for the supported bitdepths, and reconcile their
include lists with upstream's source inventory. The `x86/config.h` and `asm-windows/config.asm` serve
both Windows architectures; replacing them with a single-arch Meson output breaks the other build.
`asm-elf/config.asm` is Linux's assembly configuration, and `generic/config.h` builds dav1d's C alone
where there is no NASM. Keep the release string in `vcs_version.h`.

### libaom

Upgraded in libfiles, whose `cmake/aom/` holds its configuration and `cmake/aom.cmake` its source lists:
generate both again with the release's own CMake, as libfiles'
[design](https://github.com/ZacWalk/libfiles/blob/main/docs/design.md#codecs) records, and reconcile the
lists with the release's. Verify the new archive's signature against AOMedia's release key, and bring
the licence and patent texts in [licences](../licences) up to the release's `LICENSE` and `PATENTS`.
An upgrade runs libfiles' AVIF tests and the `*AVIF*` tests here, in Release, on x64 and x86: the x86-64
build takes libaom's SIMD and the 32-bit one its C alone.

### libopenmpt

A new archive pin, of the `.autotools` source release. Build `common/`, `libopenmpt/`, `sounddsp/`, `soundlib/`
and `src/`; `common/version.cpp` takes the release's `build/svn_version/svn_version.h` when no other is on the
include path. Do not build player plugins or test programs into the library. Native tracker playback uses its C++
and interactive interfaces directly, so it is a required dependency on both platforms, and modules reach the
player through the native PCM bridge rather than the FFmpeg module demuxer. That demuxer must still remain present
on both platforms: both FFmpeg recipes name the fetched tree, which is why [CMakeLists.txt](../CMakeLists.txt)
fetches it before any recipe runs.

## Faces

libmug owns face detection and recognition, with the networks, their pins and their conversion:
libfacedetection's YuNet weights and the OpenCV Zoo's SFace, converted as libmug's build runs - with
only Python's standard library, so the build needs a Python 3 interpreter - and run on libmug's own
kernels. Its [design](https://github.com/ZacWalk/libmug/blob/main/docs/design.md#the-networks) says how
each is stored, and its tests hold its detector to libfacedetection's and its vectors to OpenCV's
FaceRecognizerSF. Upgrading either network is libmug's change; here it keeps these:

- Highway is libfiles', named to libmug as `LIBMUG_HIGHWAY_TARGET`, so the application links one copy.
- The face tests read libfacedetection's sample photographs from the archive libmug fetches, at
  `mobile/Android/FaceDetection/app/src/main/assets/`; an upgrade that moves them moves the tests' path
  in [test_faces.cpp](../src/test_faces.cpp) and in [test_mcp_session.py](../tools/test_mcp_session.py).
- A new network moves libmug's model id, and so the one every face record carries: the collection's
  faces are read again.

## Assets

The Fluent UI icon font and generated code-point table are one versioned asset. Updating the font
requires the matching upstream JSON and regeneration through `tools/fluent_icons.py`; code points are
not edited by hand.

| Asset | Location | Upstream / update rule |
| --- | --- | --- |
| Fluent UI System Icons | `src/Res/FluentSystemIcons-Resizable.ttf` | [Microsoft upstream](https://github.com/microsoft/fluentui-system-icons); take the font and JSON from one revision and regenerate [app_icons.h](../src/app_icons.h). The font's internal `1.0` is not a release identifier. |
| Location data | `exe/location-countries.txt`, `exe/location-states.txt`, `exe/location-places.txt` | [GeoNames dumps](https://download.geonames.org/export/dump/) and the country-name source used by `tools/generate_locations.py`; regenerate as a set. |
| Spelling dictionaries | `exe/dictionaries` | Keep each dictionary's upstream provenance, encoding, paired affix file and redistribution notices when replacing it. |

Location data and dictionaries retain their own upstream notices and generation procedures. They are
packaged data, not source dependencies, but the same redistribution rule applies.

## Where this lives

- [dd-dependencies.json](../cmake/dd-dependencies.json): the pinned archives, and
  [DiffractorFetch.cmake](../cmake/DiffractorFetch.cmake), which fetches them.
- [DiffractorLibdesktop.cmake](../cmake/DiffractorLibdesktop.cmake),
  [DiffractorLibfiles.cmake](../cmake/DiffractorLibfiles.cmake), [DiffractorLib64.cmake](../cmake/DiffractorLib64.cmake)
  and [DiffractorLibmug.cmake](../cmake/DiffractorLibmug.cmake): the sibling libraries' pinned commits and the
  checkouts built in their place; [DiffractorHeif.cmake](../cmake/DiffractorHeif.cmake): libheif's FFmpeg decoder
  plugin.
- `cmake/vendored/`: owned source selection, definitions, and generated configuration.
- [DiffractorDependency.cmake](../cmake/DiffractorDependency.cmake) and
  [DiffractorCompilerPolicy.cmake](../cmake/DiffractorCompilerPolicy.cmake): common dependency and
  compiler policy.
- [files.cpp](../src/files.cpp), the decoders gated on their libraries
  ([files_heif.cpp](../src/files_heif.cpp), [files_jxl.cpp](../src/files_jxl.cpp) and
  [files_raw.cpp](../src/files_raw.cpp)), [av_format.cpp](../src/av_format.cpp),
  [metadata.cpp](../src/metadata.cpp), [util_spell.cpp](../src/util_spell.cpp) and
  [util_zip.cpp](../src/util_zip.cpp): application-owned wrappers.
- `tools/compare_ffmpeg_config.py` and `tools/fluent_icons.py`: reproducible integration checks and
  generators.
- [ffmpeg_msvc.cmake](../cmake/vendored/ffmpeg_msvc.cmake) and
  [ffmpeg_msvc.local.cmake](../cmake/vendored/ffmpeg_msvc.local.cmake): Windows source inventory,
  assembly configuration and architecture exclusions; `cmake/vendored/ffmpeg-windows/`: the
  configuration FFmpeg's configure generated for Windows.
- [test_files.cpp](../src/test_files.cpp), [test_metadata.cpp](../src/test_metadata.cpp),
  [test_av.cpp](../src/test_av.cpp), [test_text.cpp](../src/test_text.cpp),
  [test_util.cpp](../src/test_util.cpp) and [test_faces.cpp](../src/test_faces.cpp):
  application-level format, metadata, media, text, face detection and face recognition checks.
- `dd.ps1` (`Copy-LicenceTexts`) and `installer/diff.nsi`: the licence texts each release package
  carries - the LGPL and GPL texts, libaom's licence and patent licence, and the licences of the
  libfacedetection and SFace weights libmug compiles in.
