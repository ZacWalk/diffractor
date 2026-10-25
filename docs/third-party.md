# Third-Party Dependencies

This document owns the dependency inventory, upstream sources, licensing, vendoring policy, and
integration constraints that an upgrade must preserve. The inventory records the imported releases
and snapshots; the pins in [dd-dependencies.json](../cmake/dd-dependencies.json) and the revisions
the libfiles and libdesktop modules name are authoritative, and CMake owns the exact compiled source
lists. Wrapper behavior belongs to the corresponding subject document.

## Licensing

Diffractor source is LGPL-2.1-or-later. Released binaries are LGPL-3.0-or-later because they
statically link libheif and libde265, which are LGPL-3.0-or-later. The repository root contains the
LGPL and incorporated GPL texts, and every release package - the installer, the portable zip and the
Store package - carries both beside the executable, as the LGPL requires of each copy.

FFmpeg is configured without GPL, version-3, or LGPL-v3-only components, so it remains usable under
LGPL 2.1. Enabling a GPL component changes the outbound license of the combined binary and is not an
ordinary build option.

A dependency or model with non-commercial terms, no redistribution grant, or incompatible
additional restrictions cannot ship. Every dependency must retain the license and notices required
by its exact upstream version; the license file in its pinned archive is the authority for those
terms.

## Vendoring

Nothing is vendored. Every dependency is an archive CMake fetches at the version
[dd-dependencies.json](../cmake/dd-dependencies.json) pins by SHA-256 - Diffractor's own, the FFmpeg
fork's commit among them - or one libfiles or libdesktop fetches at its own pin; assets live in their
consuming tree. Owned CMake modules under `cmake/vendored/` select each library's sources,
definitions and compiler policy, and keep beside them the configuration headers a library's own build
would have generated.

Fetched source is upstream material, never patched where it lands. Keep Diffractor changes in
wrappers, CMake modules, or the maintained fork; a configuration header kept in `cmake/vendored/` is
part of its library's upgrade checklist.

## Dependency Inventory

Update this table with each pin. A snapshot is an exact upstream commit, not a moving `main`
branch or the API version left in its headers. FFmpeg is a maintained fork, not an interchangeable
upstream release archive.

| Library | Imported release or upstream snapshot | Location | Type | Upstream / update source |
| --- | --- | --- | --- | --- |
| Brotli | 1.2.0 | Fetched by libfiles | Release archive by SHA-256 | [google/brotli releases](https://github.com/google/brotli/releases). libfiles pins the archive in its `cmake/dd-dependencies.json` and builds its C decoder and encoder, every source this tree's vendored copy built, as `libfiles::brotli`; [CMakeLists.txt](../CMakeLists.txt) names it `diffractor::brotli`, which libjxl and [files_jxl.cpp](../src/files_jxl.cpp), for a JPEG XL's compressed metadata boxes, link |
| bzip2 | 1.0.8 | Fetched | Release archive by SHA-256 | [Sourceware](https://sourceware.org/bzip2/downloads.html). Pinned in [dd-dependencies.json](../cmake/dd-dependencies.json) and built unchanged by [bzip2.cmake](../cmake/vendored/bzip2.cmake): the copy once vendored here spelled four POSIX names the MSVC way, which the vendored policy's `_CRT_NONSTDC_NO_WARNINGS` makes needless |
| dav1d | 1.5.4 | Fetched by libfiles | Release archive by SHA-256 | [VideoLAN](https://code.videolan.org/videolan/dav1d). libfiles pins the release archive in its `cmake/dd-dependencies.json` and builds it with the source list, configuration and bitdepth-template wrappers this tree's vendored copy used, as `libfiles::dav1d`; [CMakeLists.txt](../CMakeLists.txt) names it `diffractor::dav1d`, which libheif and FFmpeg's AV1 decoder link |
| Adobe DNG SDK | 1.7.1 build 2724 | Fetched by libfiles | Release archive by SHA-256 | [Adobe source archive](https://download.adobe.com/pub/adobe/dng/dng_sdk_1_7_1_2724_20260908.zip). libfiles pins the archive in its `cmake/dd-dependencies.json` and builds `dng_sdk/source/` - every source but the SDK's XMP backend, which its [dng_xmp_sdk.cpp](https://github.com/ZacWalk/libfiles/blob/main/src/dng_xmp_sdk.cpp) replaces - with the `RawEnvironment.h` this tree's copy carried, as `libfiles::dng`; [CMakeLists.txt](../CMakeLists.txt) names it `diffractor::dng`, which LibRaw hands DNGs to |
| FFmpeg | `eea8d39cc97af0ab398c23a2b2dc45efd8148159`: upstream `7366b2f552c929337a7cf14c3ed18b55ddfdbab3` and Diffractor's commits | Fetched | Fork commit archive by SHA-256 | [Diffractor fork](https://github.com/ZacWalk/FFmpeg), based on [upstream FFmpeg](https://github.com/FFmpeg/FFmpeg). Pinned in [dd-dependencies.json](../cmake/dd-dependencies.json); [FFmpeg.cmake](../cmake/vendored/FFmpeg.cmake) drives its configure and make on Linux, and [ffmpeg_msvc.cmake](../cmake/vendored/ffmpeg_msvc.cmake) builds it on Windows with the configuration the fork checks in |
| Highway | 1.4.0 | Fetched by libfiles | Release archive by SHA-256 | [google/highway releases](https://github.com/google/highway/releases). libfiles pins the archive and builds it with the source list this tree's vendored copy used, as `libfiles::highway`, which libjxl alone uses |
| Hunspell | 1.7.4 | Fetched | Release archive by SHA-256 | [Hunspell releases](https://github.com/hunspell/hunspell/releases). Pinned in [dd-dependencies.json](../cmake/dd-dependencies.json); [hunspell.cmake](../cmake/vendored/hunspell.cmake) builds its `src/hunspell/` unchanged, the version and visibility headers included |
| libarchive | 3.8.9 | Fetched | Release archive by SHA-256 | [libarchive releases](https://github.com/libarchive/libarchive/releases). Pinned in [dd-dependencies.json](../cmake/dd-dependencies.json); [archive.cmake](../cmake/vendored/archive.cmake) builds it unchanged, with the configuration its configure produced for Windows and for Linux kept in `cmake/vendored/archive-windows/` and `archive-linux/` |
| libde265 | 1.1.3 | Fetched by libfiles | Release archive by SHA-256 | [strukturag/libde265 releases](https://github.com/strukturag/libde265/releases). libfiles pins the archive and builds it with the source list and generated `config.h` and `de265-version.h` this tree's vendored copy used, as `libfiles::de265`, libheif's HEVC decoder where FFmpeg's is not named |
| libdesktop | `DIFFRACTOR_LIBDESKTOP_REVISION` in [DiffractorLibdesktop.cmake](../cmake/DiffractorLibdesktop.cmake) | Fetched by CMake | Shared library, LGPL-2.1-or-later | [ZacWalk/libdesktop](https://github.com/ZacWalk/libdesktop), its system and render modules only; a `../libdesktop` checkout is built instead when present. Owns the foundation (`df::`, `str::`), the OS services (`platform.h`) and the desktop UI (`ui.h`, the window layer and the renderers) Diffractor is written in, with their Windows and Linux backends |
| libfiles | `DIFFRACTOR_LIBFILES_REVISION` in [DiffractorLibfiles.cmake](../cmake/DiffractorLibfiles.cmake) | Fetched by CMake | Shared library, LGPL-2.1-or-later | [ZacWalk/libfiles](https://github.com/ZacWalk/libfiles), shared with ImageWalker; a `../libfiles` checkout is built instead when present. Owns the XMP data model, the still-image scanners (JPEG, PNG, WebP, GIF, TIFF, PSD, BMP, HEIF/AVIF, JPEG XL) and the camera raw scan, the JPEG, PNG, WebP, PSD, HEIF/AVIF and JPEG XL decoders, which write into the surface Diffractor allocates - a HEIF thumbnail item included - the camera raw loader, which hands back a raw file's JPEG preview or writes its bitmap preview or developed image into that surface, and the Commodore 64 listing; libpng and libwebp now only encode, libheif is called here only to register its FFmpeg decoder plugin, and libjxl only by the DNG SDK |
| libheif | 1.23.5 | Fetched by libfiles | Release archive by SHA-256 | [strukturag/libheif releases](https://github.com/strukturag/libheif/releases). libfiles pins the archive and builds its library subtree with the JPEG, libde265, dav1d and uncompressed decoders, as `libfiles::heif`, which libfiles' HEIF decoder uses; its FFmpeg decoder plugin is built here, from that tree, by [DiffractorHeif.cmake](../cmake/DiffractorHeif.cmake), and registered by [files_heif.cpp](../src/files_heif.cpp), which names it to libfiles for HEVC images |
| libjpeg-turbo | 3.2.0 | Fetched by libfiles | Release archive by SHA-256 | [libjpeg-turbo releases](https://github.com/libjpeg-turbo/libjpeg-turbo/releases). libfiles pins the archive in its `cmake/dd-dependencies.json` and builds it with the recipe this tree's vendored copy used, as `libfiles::jpeg`, and decodes, encodes and losslessly turns JPEGs with it; [CMakeLists.txt](../CMakeLists.txt) names it `diffractor::jpeg`, which the codecs that read JPEG data and the tests link |
| libjxl | 0.12.0 | Fetched by libfiles | Release archive by SHA-256 | [libjxl releases](https://github.com/libjxl/libjxl/releases). libfiles pins the archive and builds its decoder, encoder, colour management and thread runners - the source list this tree's vendored copy used, less the `extras` nothing linked - with the version and export headers its own build generates, as `libfiles::jxl`, and decodes JPEG XL with it; [CMakeLists.txt](../CMakeLists.txt) names it `diffractor::jxl`, which the DNG SDK links. It and Highway were built unoptimised under MSVC for years, for a miscompile that decoded a JPEG XL to an empty image; with these versions and MSVC 14.51 it does not happen, so they take the release policy's `/O2` and `/GL` like the rest, and decode some fifty times faster |
| XZ / liblzma | 5.8.4 | Fetched | Release archive by SHA-256 | [XZ releases](https://github.com/tukaani-project/xz/releases). Pinned in [dd-dependencies.json](../cmake/dd-dependencies.json); [lzma.cmake](../cmake/vendored/lzma.cmake) builds `src/liblzma/` and `src/common/` unchanged, not the command-line tools, with the `config.h` the copy once vendored here carried, kept in `cmake/vendored/lzma/` |
| libopenmpt | 0.8.9 | Fetched | Release archive by SHA-256 | [libopenmpt downloads](https://lib.openmpt.org/libopenmpt/download/). Pinned in [dd-dependencies.json](../cmake/dd-dependencies.json), the `.autotools` source release; [openmpt.cmake](../cmake/vendored/openmpt.cmake) builds it unchanged, with the release's own `svn_version.h` |
| libpng | 1.6.59 | Fetched by libfiles | Release archive by SHA-256 | [libpng](https://www.libpng.org/pub/png/libpng.html), [upstream tags](https://github.com/pnggroup/libpng/tags). libfiles pins the archive in its `cmake/dd-dependencies.json` and builds it in C alone, as this tree's vendored copy was built, with the same `pnglibconf.h`, as `libfiles::png`; and decodes and encodes PNGs with it; [CMakeLists.txt](../CMakeLists.txt) names it `diffractor::png`, which the tests link |
| LibRaw | 0.22.2 | Fetched by libfiles | Release archive by SHA-256 | [LibRaw releases](https://github.com/LibRaw/LibRaw/releases). libfiles pins the archive and builds its library's sources - the list this tree's vendored copy used - with the DNG SDK and libjpeg, as `libfiles::raw`, and loads raw files with it - their preview, or their raw data developed - and scans them - their metadata, the info pane's listing and a thumbnail; [CMakeLists.txt](../CMakeLists.txt) names it `diffractor::raw`, which diffractor links for libfiles and no longer calls itself |
| libwebp | 1.6.0 | Fetched by libfiles | Release archive by SHA-256 | [libwebp tags](https://github.com/webmproject/libwebp/tags). libfiles pins the archive in its `cmake/dd-dependencies.json` and builds its decoder, encoder, sharpyuv, mux and demux as one library, from the source list this tree's vendored copy used, as `libfiles::webp`; and decodes still WebPs and encodes WebPs with it; [CMakeLists.txt](../CMakeLists.txt) names it `diffractor::webp`, which the tests link |
| minizip-ng | 4.2.2 | Fetched | Tag archive by SHA-256 | [minizip-ng releases](https://github.com/zlib-ng/minizip-ng/releases). Pinned in [dd-dependencies.json](../cmake/dd-dependencies.json); [minizip.cmake](../cmake/vendored/minizip.cmake) builds it unchanged, the `compat/` API included, with the `mz_config.h` the copy once vendored here carried, kept in `cmake/vendored/minizip/` |
| RapidJSON | `24b5e7a8b27f42fa16b96fc70aade9106cf7102f` | Fetched by libdesktop | Commit archive by SHA-256 | [Tencent/rapidjson](https://github.com/Tencent/rapidjson). libdesktop pins the archive in its `cmake/dd-dependencies.json` and gives its headers to `libdesktop::system`'s consumers, which [model_movie.cpp](../src/model_movie.cpp) includes; nothing of it is built |
| skcms | `7e572ada698a60ff50bd9038fd6e01e44f40868a` | Fetched by libfiles | Git commit | [Skia upstream](https://skia.googlesource.com/skcms/). It makes no releases, and Skia's generated archives differ from one download to the next, so libfiles pins the commit, clones it, and builds its baseline, Haswell and Skylake units as `libfiles::skcms`, which libjxl's colour management uses |
| SQLite | 3.53.4 | Fetched | Amalgamation archive by SHA-256 | [sqlite.org](https://www.sqlite.org/download.html). Pinned in [dd-dependencies.json](../cmake/dd-dependencies.json); [sqlite.cmake](../cmake/vendored/sqlite.cmake) builds the amalgamation unchanged, with Diffractor's compile-time options |
| zlib-ng | 2.3.3, zlib-compatible API | Fetched by libfiles | Release archive by SHA-256 | [zlib-ng releases](https://github.com/zlib-ng/zlib-ng/releases), not stock zlib. libfiles pins the archive in its `cmake/dd-dependencies.json` and builds it with the recipe and generated headers this tree's vendored copy used, as `libfiles::zlib`; [CMakeLists.txt](../CMakeLists.txt) names it `diffractor::zlib`, which libpng, libarchive, minizip, the DNG SDK and FFmpeg link |

Version traps matter during an audit: RapidJSON's `1.1.0` does not identify a post-release snapshot, and zlib-ng's `1.3.1.zlib-ng` describes its compatibility API.
Use the SDK build number for DNG, the public `hunversion.h` for Hunspell, and `ZLIBNG_VERSION`
in the generated `zlib.h` - libfiles' `cmake/zlib-ng/zlib.h` - for zlib-ng rather than stale package
strings or input templates.

### Build Tools

| Tool | Version | Location | Upstream / retained notice |
| --- | --- | --- | --- |
| NASM | 3.02 | Fetched while configuring on Windows when none is on the path | [NASM Windows archives](https://www.nasm.us/pub/nasm/releasebuilds/3.02/), pinned in [dd-dependencies.json](../cmake/dd-dependencies.json) - libfiles' pin; its `LICENSE` is in the archive |
| 7-Zip Extra | 26.03 | Fetched by `dd.ps1` into `tmp/tools/7zip/` | [7-Zip release](https://github.com/ip7z/7zip/releases/tag/26.03): `7zr.exe` unpacks `7z2603-extra.7z`, both pinned in [dd-dependencies.json](../cmake/dd-dependencies.json); its `License.txt` is in the archive |
| SymStore | Installed Windows SDK | `Windows Kits\10\Debuggers\x64\symstore.exe` | Machine-installed SDK, or a per-user SDK administrative image as described in [crash.md](crash.md#3-symbols); packaging checks it before changing release state |

`dd.ps1` packs with the release's x86 `7za.exe` and its two DLLs, as `tools/` once carried. SignTool
and SymStore come from the installed Windows SDK. These are build and packaging tools, not additional media
decoders. Installed Visual Studio,
CMake, Ninja, Windows SDK and NSIS prerequisites are described in [README](../README.md#building).

## Upgrade Procedure

1. Identify the exact upstream release or commit and verify its license.
2. Pin it: the archive's URL and SHA-256 in [dd-dependencies.json](../cmake/dd-dependencies.json),
   or in libfiles' or libdesktop's pins for a library one of them owns. Compare the new release's
   source inventory with the old one's.
3. Reapply the integration constraints below - a configuration header kept in `cmake/vendored/` is
   brought up to the release - update the owning CMake module's source list, and update the
   inventory. Record snapshot commit IDs and DNG build numbers, not just a branch or API version.
4. Configure from a clean build directory so removed sources cannot survive as stale objects.
5. Build Debug and Release x64 plus Release Win32 where the dependency has architecture-specific
   code, then run `.\dd.ps1 test` and its focused wrapper tests.

Build only what the application needs from a fetched archive: not upstream tests, benchmarks,
examples or build systems. CMake source lists are explicit; a file in a fetched archive is not part
of Diffractor until a module names it.

## XMP Toolkit

Diffractor does not use Adobe's XMP Toolkit, and nothing in it needs Expat: libarchive's XAR reader
is built without an XML parser. Releases up to 1.27.2 built a fork of the toolkit as the
`third-party/xmp` submodule; that fork is no longer published, so those tags do not build from a
fresh clone.

[libfiles](https://github.com/ZacWalk/libfiles) provides what the toolkit did. The XMP data model -
parsing, editing and serializing packets - is libfiles', and so is reading and writing the packet in
every file Diffractor embeds metadata in: the still formats (JPEG, PNG, GIF, WebP, TIFF and PSD) with
their EXIF and IPTC, MP4 and QuickTime with their Xtra, `cprt` and `mvhd` fields, ASF with its
header's Content Description, WM/Category, WM/SharedUserRating and File Properties date, RIFF with its
INFO list, MPEG audio with its ID3v2 frames, and FLV. A format conversion reads the source's packet
the same way - or, for a format libfiles does not read, such as HEIF, the packet the scan finds - and
fills what the packet lacks from what the item shows, as the toolkit's import of legacy metadata did.
JPX and WBMP, which nothing Diffractor uses can write, keep their metadata in an `.xmp` sidecar, as raw
files do.

The DNG SDK's XMP backend, `dng_xmp_sdk`, is libfiles'
[dng_xmp_sdk.cpp](https://github.com/ZacWalk/libfiles/blob/main/src/dng_xmp_sdk.cpp), which implements the SDK's own `dng_xmp_sdk.h` over libfiles' XMP data model and is
built in place of the SDK's `dng_xmp_sdk.cpp` (see the DNG SDK row below).

libfiles reads a packet as the toolkit did; its design document records
[how it behaves](https://github.com/ZacWalk/libfiles/blob/main/docs/design.md#how-it-behaves) and
where it differs on purpose. test_metadata holds libfiles' reading of every packet in the fixtures,
whole and truncated, and its serialization of them, steady; before the toolkit was removed, the same
tests held the two to an identical reading and byte-identical serialization. The fork's integration
differences are observable metadata behavior, and libfiles carries each of them:

- WebP metadata rewrites preserve physical chunk order and payload boundaries.
- MP3 POPM ratings and TPE2 album artists, and the Windows keyword and rating fields of MP4 (Xtra) and
  ASF (WM/Category, WM/SharedUserRating), match the properties described in [metadata.md](metadata.md).
- Unknown metadata is preserved, and a legacy field is not deleted merely because an unrelated XMP
  property is absent; libfiles writes only the fields an edit sets.
- An unchanged Xtra box is left as it is, and MP4 updates patch or append without moving the media.

The write path and rollback boundary are owned by [file-io.md](file-io.md). A successful build is
insufficient; metadata and format round-trip tests prove these behaviors remain effective.

## FFmpeg

FFmpeg is the Diffractor fork, [ZacWalk/FFmpeg](https://github.com/ZacWalk/FFmpeg), fetched at the
commit [dd-dependencies.json](../cmake/dd-dependencies.json) pins: a rebase is a new commit on the fork
and a new pin here. The fork's configuration controls available demuxers,
decoders, parsers, hardware acceleration, and metadata keys. It builds no end-user programs and does
not enable network protocols for application media access. It is configured for decode speed rather
than size, which also keeps codec profile names; it decodes software AV1 through libdav1d, built
against libfiles' dav1d; D3D11VA is its only hardware acceleration; and it has no muxers or
devices.

Rebasing must preserve metadata-key normalization and the bounded metadata-probe behavior consumed by
[av_format.cpp](../src/av_format.cpp). Architecture dispatch must use real compiled kernels or exclude
foreign-architecture sources; unresolved SIMD symbols must never be answered by no-op stubs.

Windows configuration remains architecture-specific. Linux uses the fork's configure/build system
rather than restating its codec inventory in application CMake.

### FFmpeg Configuration And Rebase Checklist

- Inventory the functional difference from the old upstream base before merging or rebasing.
  Preserve ASF/AVI/FLV/WAV embedded XMP extraction, repeated ASF categories, RIFF metadata keys,
  MP4 Windows Xtra tags and ratings, and the distinction between iTunes content advisory and star
  rating. Preserve H.264 stream characterization during discard-all probing and the audio
  reallocation guards. These changes can be folded into a commit that otherwise looks generated.
- Configure an LF-only source export in a separate build directory. For a Windows cross-configure,
  use MinGW with the matching x86 or x64 target, runtime CPU detection, external x86 assembly,
  no inline assembly, Windows threads, and zlib. Disable programs, documentation, network,
  encoders, muxers, devices and libavdevice, DXVA2, filters and general protocols; retain the
  `file` protocol. Enable libdav1d and do not pass `--enable-small`, which strips profile names
  and trades decode speed for size. Do not enable GPL, version-3 or nonfree components.
- Keep the `config.h` and `config.asm` dispatchers. Regenerate `config-x64.h`, `config-x86.h` and
  their assembly equivalents independently. MinGW compiler and libc probes are not MSVC probes:
  preserve the MSVC capability overrides, disable GNU inline assembly, and keep
  `HAVE_ALIGNED_STACK` disabled on Win32. MSVC does not supply `unistd.h`; do not accidentally
  select the application compatibility header instead of FFmpeg's native Windows I/O support.
- Reconcile the already-linked bzip2, liblzma and libopenmpt backends when the cross-configure
  cannot detect them. libopenmpt needs both its component switch and its entry in the generated
  demuxer list. A switch without a compiled implementation is not support.
- Generate the shared component headers and codec, parser, bitstream-filter, demuxer, muxer,
  protocol and device lists. Verify the shared files agree between architectures. Generate
  `libswscale/x86/uops_macros.gen.asm` with the upstream host compiler on x64; it is an include,
  not a standalone assembly unit. Stamp `libavutil/ffversion.h` with the upstream revision.
- Derive source changes from complete `make -Bn` dry runs for both architectures, not an
  incremental `make -n`. Update [ffmpeg_msvc.cmake](../cmake/vendored/ffmpeg_msvc.cmake), remove
  deleted source names, and retain assembly include paths, unique object names and Win32
  exclusions. The atomics compatibility layer needs both `compat/atomics` and
  `compat/atomics/win32` on the MSVC include path.
- Compare the Windows/Linux component sets with `tools/compare_ffmpeg_config.py`. Explain any
  intentional platform difference in `cmake/vendored/ffmpeg-config-divergence.txt`; do not simply
  accept a changed baseline. A working Windows build does not verify the Linux configuration.
- Prove metadata reconciliation and bounded probing through the application wrapper tests.
  Run the metadata, video, audio and format tests, then the complete suite. Do not replace a
  missing assembly implementation with an empty symbol or drop a decoder to make the link pass.

## Image And Archive Libraries

Image decoders, encoders, compression libraries, SQLite, and archive readers are built through their
owned CMake modules. Keep generated configuration multi-architecture where one source tree serves
x86 and x64. Preserve application wrappers as the sole boundary for errors, allocation limits,
pixel formats, and cancellation; vendor callbacks must not escape those contracts.

An upgrade that changes a file format's metadata, orientation, color, alpha, or malformed-input
behavior requires the corresponding format tests, not only a compile.

### Source Subsets And Generated Files

| Dependency | Integration that an import must preserve |
| --- | --- |
| libpng / zlib-ng | Upgraded in libfiles: a new archive pin, with the headers their own builds generate - libpng's `pnglibconf.h`, zlib-ng's `zlib.h`, `zconf.h` and `zlib_name_mangling.h`, in libfiles' `cmake/` - brought up to the release. Use the upstream source lists to identify added or removed units; zlib-ng's per-instruction-set sources need their compiler flags. libpng is built in C alone, as it always was here; turning on its SIMD filters is a change to measure, not part of an upgrade. |
| libheif / libde265 | Upgraded in libfiles: a new archive pin, with the headers their own builds generate - libheif's `heif_version.h`, libde265's `config.h` and `de265-version.h`, in libfiles' `cmake/` - brought up to the release. Keep static linkage, the selected decoder backends and libde265's platform configuration; do not enable additional encoder/plugin dependencies implicitly. Keep libheif's FFmpeg decoder plugin, which [DiffractorHeif.cmake](../cmake/DiffractorHeif.cmake) builds from libfiles' libheif: [files_heif.cpp](../src/files_heif.cpp) registers it and names it for HEVC images, which it decodes faster than libde265 and, unlike libde265's 32-bit build, bit-exactly. |
| libjpeg-turbo | Upgraded in libfiles: a new archive pin, with the configuration headers its `cmake/libjpeg-turbo/` carries for MSVC and for GCC and Clang brought up to the release. Use the upstream source lists to identify added or removed units, and preserve the high-bit-depth libraries. |
| libjxl / Highway / Brotli | Upgraded in libfiles: a new archive pin, with the version and export headers libjxl's own build generates, in libfiles' `cmake/jxl/include/`, brought up to the release. Use the upstream source lists to identify added or removed units, and preserve libjxl's Highway, Brotli and skcms dependencies. An upgrade runs the JPEG XL tests in Release, optimised: "Should scan jxl metadata" is where an empty JPEG XL - the miscompile libjxl and Highway were once built unoptimised for - would show. |
| liblzma | A new archive pin. Keep `cmake/vendored/lzma/config.h` and the Linux POSIX-thread override, and build the library and common subset, not the differently licensed command-line scripts and tools. |
| libarchive | A new archive pin. Keep `cmake/vendored/archive-windows/config_windows.h` and `archive-linux/config_linux.h`, which `PLATFORM_CONFIG_H` names, and bring them up to the release's configure output for each platform. |
| minizip-ng | A new archive pin. Keep `cmake/vendored/minizip/mz_config.h`, the configuration Windows and Linux both build with, and bring it up to the release's `mz_config.h.cmakein`. Its zlib-style compatibility API lives in `compat/`; retain it and the platform-specific crypto selection. |
| Hunspell | A new archive pin. Keep the static-library configuration. The source list includes `hunspelltrace.cxx`; dropping it leaves the trace hooks unresolved. |
| LibRaw / DNG SDK | Upgraded in libfiles: a new archive pin each, with the SDK's `RawEnvironment.h`, in libfiles' `cmake/dng/`, kept. Preserve the LibRaw wrapper boundary. Build only the SDK's `dng_sdk/source/`, not Adobe's bundled copies of libraries libfiles owns. The SDK's `dng_xmp_sdk.cpp` is not built: libfiles' [dng_xmp_sdk.cpp](https://github.com/ZacWalk/libfiles/blob/main/src/dng_xmp_sdk.cpp) is built in its place, so an upgrade that changes `dng_xmp_sdk.h` or `dng_xmp_sdk.cpp` must carry the change there - the namespaces `InitializeSDK` registers, which faults each call reports, swallows or throws, and the toolkit utilities it reproduces. The SDK is built with `qDNGXMPFiles=0`, and document-history support remains disabled, because that backend has neither XMPFiles nor `SXMPDocOps`. |
| skcms | Upgraded in libfiles: a new commit pin. Keep the baseline, Haswell and Skylake transform units and their compiler-specific instruction-set flags. Validate ICC transforms, not only compilation. |
| RapidJSON | Upgraded in libdesktop: a new archive pin. Its upstream `bin/jsonchecker/` has a different, restrictive license, and nothing of it is built or shipped. |

### dav1d

Upgraded in libfiles, whose `cmake/dav1d/` holds what an import used to carry here. Keep the four
bitdepth-template wrappers that compile the upstream template sources for the supported bitdepths,
and reconcile their include lists with upstream's source inventory. The `x86/config.h` and
`asm-windows/config.asm` serve both Windows architectures; replacing them with a single-arch Meson
output breaks the other build. `asm-elf/config.asm` is Linux's assembly configuration, and
`generic/config.h` builds dav1d's C alone where there is no NASM. Keep the release string in
`vcs_version.h`.

### libopenmpt

A new archive pin, of the `.autotools` source release. Build `common/`, `libopenmpt/`, `sounddsp/`,
`soundlib/` and `src/`; `common/version.cpp` takes the release's `build/svn_version/svn_version.h`
when no other is on the include path. Do not build player plugins or test programs into the library.
Its FFmpeg demuxer must remain present on both platforms: both FFmpeg recipes name the fetched tree,
which is why [CMakeLists.txt](../CMakeLists.txt) fetches it before any recipe runs.

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
- `cmake/vendored/`: owned source selection, definitions, and generated configuration.
- [DiffractorDependency.cmake](../cmake/DiffractorDependency.cmake) and
  [DiffractorCompilerPolicy.cmake](../cmake/DiffractorCompilerPolicy.cmake): common dependency and
  compiler policy.
- [files_core.cpp](../src/files_core.cpp), the `files_*` decoders,
  [av_format.cpp](../src/av_format.cpp), [metadata_xmp.cpp](../src/metadata_xmp.cpp),
  [util_spell.cpp](../src/util_spell.cpp), and [util_zip.cpp](../src/util_zip.cpp): application-owned
  wrappers.
- `tools/compare_ffmpeg_config.py` and `tools/fluent_icons.py`:
  reproducible integration checks and generators.
- [ffmpeg_msvc.cmake](../cmake/vendored/ffmpeg_msvc.cmake) and
  [ffmpeg_msvc.local.cmake](../cmake/vendored/ffmpeg_msvc.local.cmake): Windows source inventory,
  assembly configuration and architecture exclusions.
- [test_files.cpp](../src/test_files.cpp), [test_metadata.cpp](../src/test_metadata.cpp),
  [test_av.cpp](../src/test_av.cpp), [test_text.cpp](../src/test_text.cpp) and
  [test_util.cpp](../src/test_util.cpp): application-level format, metadata, media and text checks.
- `dd.ps1` (`Copy-LicenceTexts`) and `installer/diff.nsi`: the licence texts each release package
  carries.
