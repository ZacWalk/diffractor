# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: The build of Diffractor, for every platform, and the only description of it. df.sln and
# the .vcxproj files it named are deleted; the Visual Studio projects that remain useful for
# debugging and profiling are generated from here on demand.
#
# The source list is deliberately explicit rather than a glob, so that what is built is reviewable
# and a file cannot join the build by being dropped in a folder.
#
# Drive this through the gateway rather than by hand:
#
#   dd setup       install the build tooling and system packages
#   dd build       configure and build
#   dd run         run what has been built

cmake_minimum_required(VERSION 3.22)

# Selecting the MSVC runtime by property rather than by editing flag strings needs this policy.
cmake_policy(SET CMP0091 NEW)

project(diffractor CXX C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The architecture being built FOR. CMAKE_SYSTEM_PROCESSOR answers for the host on Windows, so it
# cannot decide whether nasm applies or whether the 32-bit SSE2 switch is meaningful: a Win32 build
# on an x64 machine and an ARM64 build both read as AMD64. The generator platform is the answer for
# a Visual Studio generator; MSVC sets the compiler architecture id for Ninja.
if (MSVC AND CMAKE_VS_PLATFORM_NAME)
    set(_diffractor_arch "${CMAKE_VS_PLATFORM_NAME}")
elseif (MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID)
    set(_diffractor_arch "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
else ()
    set(_diffractor_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif ()

string(TOLOWER "${_diffractor_arch}" _diffractor_arch)

if (_diffractor_arch MATCHES "^(x64|amd64|x86_64)$")
    set(DIFFRACTOR_TARGET_ARCH x64)
elseif (_diffractor_arch MATCHES "^(win32|x86|i[3-6]86)$")
    set(DIFFRACTOR_TARGET_ARCH x86)
elseif (_diffractor_arch MATCHES "^(arm64|aarch64)$")
    set(DIFFRACTOR_TARGET_ARCH arm64)
else ()
    set(DIFFRACTOR_TARGET_ARCH "${_diffractor_arch}")
endif ()

# The Store configuration. In app.vcxproj it is a third configuration whose every setting matches
# Release except one define, so it is an option over Release here rather than a configuration type
# that would have to restate all of them. WINSTORE disables application-owned updates.
option(DIFFRACTOR_WINSTORE "Build the Microsoft Store variant" OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug)
endif ()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(DiffractorCompilerPolicy)
include(DiffractorDependency)

# FFmpeg's Windows build assembles its x86 SIMD with nasm, and libfiles' libjpeg-turbo and dav1d
# take the assembler enabled here. enable_language() only takes effect at directory scope, and the
# recipes are included from inside a function, so it happens here. Windows seldom has nasm on the
# path, so there the release cmake/dd-dependencies.json pins - libfiles' pin - is fetched.
#
# A build tree configured while tools/ carried a copy cached its path, and find_program does not
# look again for a cached result, even a missing one; nor does enable_language() look again for a
# compiler it recorded in the tree, so a recorded one that has gone is forgotten as well.
if (NASM_EXECUTABLE AND NOT EXISTS "${NASM_EXECUTABLE}")
    unset(NASM_EXECUTABLE CACHE)
endif ()

set(_nasm_recorded "${CMAKE_BINARY_DIR}/CMakeFiles/${CMAKE_VERSION}/CMakeASM_NASMCompiler.cmake")

if (EXISTS "${_nasm_recorded}")
    file(STRINGS "${_nasm_recorded}" _nasm_recorded_compiler REGEX "^set\\(CMAKE_ASM_NASM_COMPILER \"")

    if (_nasm_recorded_compiler MATCHES "\"([^\"]+)\"" AND NOT EXISTS "${CMAKE_MATCH_1}")
        file(REMOVE "${_nasm_recorded}")
    endif ()
endif ()

find_program(NASM_EXECUTABLE
        NAMES nasm
        DOC "The nasm assembler")

if (NOT NASM_EXECUTABLE AND CMAKE_HOST_WIN32 AND DIFFRACTOR_TARGET_ARCH MATCHES "^(x64|x86)$")
    include(DiffractorFetch)
    diffractor_fetch(nasm)
    set(NASM_EXECUTABLE "${nasm_SOURCE_DIR}/nasm.exe" CACHE FILEPATH "The nasm assembler" FORCE)
endif ()

if (NASM_EXECUTABLE AND DIFFRACTOR_TARGET_ARCH MATCHES "^(x64|x86)$")
    set(CMAKE_ASM_NASM_COMPILER "${NASM_EXECUTABLE}")
    enable_language(ASM_NASM)

    if (WIN32 AND DIFFRACTOR_TARGET_ARCH STREQUAL "x64")
        set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)
    elseif (WIN32)
        set(CMAKE_ASM_NASM_OBJECT_FORMAT win32)
    elseif (DIFFRACTOR_TARGET_ARCH STREQUAL "x64")
        set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
    else ()
        set(CMAKE_ASM_NASM_OBJECT_FORMAT elf32)
    endif ()
else ()
    message(STATUS "nasm not used for ${DIFFRACTOR_TARGET_ARCH}: the libraries that assemble their SIMD will not build")
endif ()

find_program(CCACHE_PROGRAM ccache)
if (CCACHE_PROGRAM AND NOT MSVC)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif ()

# ---------------------------------------------------------------------------------------------
# Dependencies
#
# System packages are a bring-up convenience, not the shipped configuration: turn each off with
# -DDIFFRACTOR_SYSTEM_<NAME>=OFF once the vendored copy has a CMakeLists.txt. Everything here is
# OPTIONAL while the translation units that need it are still out of the build, so a missing
# package is reported rather than fatal.
# ---------------------------------------------------------------------------------------------

# Shared with ImageWalker and fetched rather than vendored: see cmake/DiffractorLibfiles.cmake. First,
# because it brings the zlib and libjpeg-turbo the libraries below link, and the DNG SDK's XMP backend
# links libfiles itself, and a dependency can only be stated on a target that already exists.
include(DiffractorLibfiles)

# zlib-ng, libpng, libwebp, libjpeg-turbo, libheif with libde265 and dav1d, libjxl with Highway, Brotli
# and skcms, and LibRaw with Adobe's DNG SDK, come with libfiles, which fetches each release archive at its pinned SHA-256 -
# skcms, which has none, at its pinned commit - and builds it with the recipe this tree's vendored
# copy used (todo.md D8). Named like the vendored libraries, so the link list and the codecs
# reach them the same way. The system packages are no substitute: the vendored zlib was zlib-ng, not
# zlib, and libfiles and the tests use the 12- and 16-bit APIs libjpeg-turbo added in 3.0, where
# Ubuntu 24.04 ships 2.1.5. dav1d is declared here, before the forks, because FFmpeg's configure registers
# its libdav1d decoder only when it can already see the library.
add_library(diffractor::zlib ALIAS libfiles-zlib)
add_library(diffractor::png ALIAS libfiles-png)
add_library(diffractor::webp ALIAS libfiles-webp)
add_library(diffractor::jpeg ALIAS libfiles-jpeg)
add_library(diffractor::de265 ALIAS libfiles-de265)
add_library(diffractor::dav1d ALIAS libfiles-dav1d)
add_library(diffractor::jxl ALIAS libfiles-jxl)
add_library(diffractor::highway ALIAS libfiles-highway)
add_library(diffractor::brotli ALIAS libfiles-brotli)
add_library(diffractor::skcms ALIAS libfiles-skcms)
add_library(diffractor::raw ALIAS libfiles-raw)
add_library(diffractor::dng ALIAS libfiles-dng)

# Libraries fetched at their pinned releases (cmake/dd-dependencies.json) whose sources other recipes
# name, so fetched before any recipe runs.
include(DiffractorFetch)
diffractor_fetch(lzma)
diffractor_fetch(openmpt)

# Every module below was imported from a .vcxproj, so each began life carrying only that project's
# Windows configuration. All of them now build under GCC too; what that took per library lives in
# cmake/vendored/<name>.local.cmake.
diffractor_dependency(sqlite PKG_CONFIG sqlite3 VENDORED_MODULE vendored/sqlite OPTIONAL)
diffractor_dependency(archive PKG_CONFIG libarchive VENDORED_MODULE vendored/archive OPTIONAL)

# Compression and support libraries. Nothing in src/ includes these directly: they are here because
# the codecs above and below link them.
diffractor_dependency(bzip2 VENDORED_MODULE vendored/bzip2 OPTIONAL)
diffractor_dependency(lzma VENDORED_MODULE vendored/lzma OPTIONAL)

# Tracked music. Declared before the forks because FFmpeg's configure enables its libopenmpt
# demuxer only when it can already see the library, and a dependency can only be stated on a target
# that already exists.
diffractor_dependency(openmpt VENDORED_MODULE vendored/openmpt OPTIONAL)

# The fork. Never resolvable from the system: docs/third-party.md records the patches a distribution
# copy would silently discard. Not OPTIONAL: media is not a feature Diffractor is valid without, so a
# configuration that cannot build it fails here rather than producing an application that reports
# every media file as absent.
diffractor_dependency(ffmpeg FORK VENDORED_MODULE vendored/FFmpeg)

# The foundation, OS services and desktop UI Diffractor is written in, fetched from libdesktop: see
# cmake/DiffractorLibdesktop.cmake.
include(DiffractorLibdesktop)

# Codecs and utilities that gate a translation unit of their own.

# libheif decodes HEVC through FFmpeg, its FFmpeg decoder plugin built here from libfiles' libheif -
# libfiles does not take on FFmpeg - with the headers the vendored libheif compiled it with, and
# registered with libheif before the first decode (files_heif.cpp). diffractor::heif is libfiles'
# libheif, and diffractor::heif_ffmpeg that plugin.
include(DiffractorHeif)
diffractor_dependency(hunspell VENDORED_MODULE vendored/hunspell OPTIONAL)
diffractor_dependency(minizip VENDORED_MODULE vendored/minizip OPTIONAL)

diffractor_report_dependencies()

# ---------------------------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------------------------

# Translation units proved to compile under GCC. Add to this list as each one is ported; the
# roadmap for what is still missing is in docs/linux.md.
set(DIFFRACTOR_PORTABLE_SOURCES
        src/util_text.cpp
        src/crypto.cpp
        src/crypto_md5.cpp
        src/crypto_sha.cpp
        src/app_text.cpp
        src/app_settings.cpp
        src/app_util.cpp
        src/app_perf.cpp
        src/app_resources.cpp
        src/model_property.cpp
        src/model_tags.cpp
        src/model_visits.cpp
        src/model_locations.cpp
        src/metadata_icc.cpp
        src/metadata_iptc.cpp
        src/files_commodore.cpp
        src/files_core.cpp
        src/files_formats.cpp
        src/files_psd.cpp
        src/files_scan_photo.cpp
        src/files_jpeg.cpp
        src/files_png.cpp
        src/files_webp.cpp
        src/media_edit.cpp
        src/metadata_exif.cpp
        src/model.cpp
        src/model_db.cpp
        src/model_index.cpp
        src/model_index_duplicates.cpp
        src/model_index_summary.cpp
        src/model_items.cpp
        src/model_movie.cpp
        src/model_search.cpp
        src/model_tile_cache.cpp
        src/render_charts.cpp
        src/render_globe.cpp
        src/render_panorama.cpp
        src/ui_elements.cpp
        src/ui_controls.cpp
        src/ui_flex.cpp
        src/app_commands.cpp
        src/app_dialogs.cpp
        src/app.cpp
        src/app_dup_report.cpp
        src/app_movie_batch.cpp
        src/app_search.cpp
        src/app_toolbar.cpp
        src/app_workers.cpp
        src/view_batch.cpp
        src/view_edit.cpp
        src/view_import.cpp
        src/view_items.cpp
        src/view_locate.cpp
        src/view_movie.cpp
        src/view_rename.cpp
        src/view_selector.cpp
        src/view_sync.cpp
        src/view_tags.cpp
)

# Unconditional: FFmpeg and libfiles are required dependencies, so configuration has already failed
# above if either is absent.
list(APPEND DIFFRACTOR_PORTABLE_SOURCES
        src/av_format.cpp
        src/metadata_xmp.cpp
)

# Tools rather than tests: neither has a library dependency, so neither is conditional.
list(APPEND DIFFRACTOR_PORTABLE_SOURCES
        src/app_gen_docs.cpp
        src/app_validate_po.cpp
)

# Codec and utility translation units, each gated on the library it needs. There is no stand-in
# file behind these: a configuration that dropped one is missing the translation unit and its
# symbols rather than getting a failing stub.
if (TARGET diffractor::heif)
    list(APPEND DIFFRACTOR_PORTABLE_SOURCES src/files_heif.cpp)
endif ()

if (TARGET diffractor::jxl)
    list(APPEND DIFFRACTOR_PORTABLE_SOURCES src/files_jxl.cpp)
endif ()

if (TARGET diffractor::raw)
    list(APPEND DIFFRACTOR_PORTABLE_SOURCES src/files_raw.cpp)
endif ()

if (TARGET diffractor::hunspell)
    list(APPEND DIFFRACTOR_PORTABLE_SOURCES src/util_spell.cpp)
endif ()

if (TARGET diffractor::minizip)
    list(APPEND DIFFRACTOR_PORTABLE_SOURCES src/util_zip.cpp)
endif ()

# The suite itself. run_console_tests builds a view_state and registers every subject file, so this
# is all or nothing.
set(DIFFRACTOR_TEST_SOURCES
        src/test_app.cpp
        src/test_av.cpp
        src/test_files.cpp
        src/test_fixtures.cpp
        src/test_index.cpp
        src/test_locations.cpp
        src/test_media_edit.cpp
        src/test_metadata.cpp
        src/test_movie.cpp
        src/test_render.cpp
        src/test_runner.cpp
        src/test_search.cpp
        src/test_text.cpp
        src/test_util.cpp
        src/test_view.cpp
)

# The application's own assets - the index's schema and the world map - are generated into a
# translation unit rather than read from disk, so the executable stays self-contained and a resource
# cannot go missing beside it. Each is a configure dependency: file(READ) is not tracked, and editing
# one would otherwise leave the build running the old copy.
function(diffractor_embed_resource SYMBOL SOURCE OUT_TEXT)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SOURCE}")
    file(READ "${SOURCE}" _hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
    string(LENGTH "${_hex}" _hex_length)
    math(EXPR _length "${_hex_length} / 2")

    # A literal semicolon has to arrive by value; escaping one inside a quoted argument leaves
    # the backslash in the written file.
    string(ASCII 59 _t)

    set(${OUT_TEXT}
            "extern const unsigned char ${SYMBOL}[]${_t}\nextern const unsigned long ${SYMBOL}_size${_t}\nconst unsigned char ${SYMBOL}[] = {${_bytes}}${_t}\nconst unsigned long ${SYMBOL}_size = ${_length}${_t}\n"
            PARENT_SCOPE)
endfunction()

diffractor_embed_resource(diffractor_resource_sql
        "${CMAKE_SOURCE_DIR}/src/Res/create_tables.sql" _sql_text)
diffractor_embed_resource(diffractor_resource_map_png
        "${CMAKE_SOURCE_DIR}/src/Res/map-hires.png" _map_png_text)

set(_generated_resources "${CMAKE_BINARY_DIR}/generated/platform_resources.cpp")
# Written through configure_file so an unchanged resource does not restamp the file and force a
# rebuild on every configure.
file(WRITE "${_generated_resources}.in"
        "// Generated from src/Res by CMake. Do not edit.\n\n${_sql_text}\n${_map_png_text}")
configure_file("${_generated_resources}.in" "${_generated_resources}" COPYONLY)
# A table of bytes needs none of the application's headers.
set_source_files_properties("${_generated_resources}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

if (WIN32)
    list(APPEND DIFFRACTOR_TEST_SOURCES src/test_platform_win.cpp)

    set(DIFFRACTOR_PLATFORM_SOURCES
            src/platform_win_av.cpp
            src/platform_win_res.rc
            "${_generated_resources}"
    )
else ()
    set(DIFFRACTOR_PLATFORM_SOURCES
            src/platform_linux_app.cpp
            "${_generated_resources}"
    )
endif ()

add_executable(diffractor
        ${DIFFRACTOR_PORTABLE_SOURCES}
        ${DIFFRACTOR_TEST_SOURCES}
        ${DIFFRACTOR_PLATFORM_SOURCES}
)

target_include_directories(diffractor PRIVATE
        src
)

# The resource script carries the app's icons, icon font, manifest and version, and includes
# libdesktop-render's own script, whose shaders that module compiles.
if (WIN32)
    # The .rc embeds these by name, and the generator only tracks the .rc itself: editing one of them
    # otherwise leaves the compiled resource stale and the binary silently running the old copy. That
    # failure is invisible - it builds clean and the application shows last week's icon.
    # CONFIGURE_DEPENDS re-globs on every build, so adding or deleting a resource cannot leave an
    # existing build tree depending on a file that is no longer there.
    file(GLOB _rc_embedded_resources CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/src/Res/*.png"
            "${CMAKE_SOURCE_DIR}/src/Res/*.ico"
            "${CMAKE_SOURCE_DIR}/src/Res/models/*")

    set_source_files_properties("${CMAKE_SOURCE_DIR}/src/platform_win_res.rc"
            PROPERTIES OBJECT_DEPENDS "${_rc_embedded_resources}")
    libdesktop_render_resources("${CMAKE_SOURCE_DIR}/src/platform_win_res.rc")
endif ()

# Every resolved dependency is named here rather than relying on one library to pull in another.
# These are static archives, and the support libraries have no translation unit of their own that
# would otherwise put them on the line.
set(DIFFRACTOR_LINK_DEPENDENCIES "")

foreach (dep IN ITEMS
        zlib png jpeg sqlite webp archive ffmpeg
        heif heif_ffmpeg jxl raw hunspell minizip
        de265 dav1d dng openmpt
        bzip2 lzma brotli highway skcms)
    if (TARGET diffractor::${dep})
        list(APPEND DIFFRACTOR_LINK_DEPENDENCIES diffractor::${dep})
    endif ()
endforeach ()

# Archives that belong on the line but have no CMake target: FFmpeg is built by its own make, and
# naming them here is what puts them inside the rescan group below.
get_property(_grouped_archives GLOBAL PROPERTY DIFFRACTOR_GROUPED_ARCHIVES)
list(APPEND DIFFRACTOR_LINK_DEPENDENCIES ${_grouped_archives})
list(APPEND DIFFRACTOR_LINK_DEPENDENCIES libfiles::libfiles libdesktop::render)

if (MSVC)
    target_link_libraries(diffractor PRIVATE ${DIFFRACTOR_LINK_DEPENDENCIES})
else ()
    # These archives genuinely reference each other in both directions -- dng calls into jxl, raw
    # into dng and jpeg, heif into jpeg, de265 and dav1d -- and GNU ld reads each archive once, in
    # order, so a reference that points backwards along the line goes unresolved. MSVC's linker
    # does not care about order, which is why one flat list was ever enough.
    list(JOIN DIFFRACTOR_LINK_DEPENDENCIES "," _link_group)
    target_link_libraries(diffractor PRIVATE "$<LINK_GROUP:RESCAN,${_link_group}>")
endif ()

diffractor_apply_app_policy(diffractor)

find_package(Threads REQUIRED)
target_link_libraries(diffractor PRIVATE Threads::Threads)

# A dependency built as an external project is not on the target's link line as a CMake target, so
# the ordering has to be stated.
get_property(DIFFRACTOR_EXTERNAL_TARGETS GLOBAL PROPERTY DIFFRACTOR_EXTERNAL_TARGETS)
if (DIFFRACTOR_EXTERNAL_TARGETS)
    add_dependencies(diffractor ${DIFFRACTOR_EXTERNAL_TARGETS})
endif ()

# Fixtures, catalogs and dictionaries are read from the folder holding the running binary
# (platform::known_folder::running_app_folder), which is the install layout on both platforms and
# exe/ in this tree.
#
# On Windows the binary goes there, under the name the rest of the toolchain expects: dd.ps1,
# installer/diff.nsi, the MSIX staging and the symbol store all name diffractor64, diffractor32 or
# diffractor rather than asking the build what it produced. Elsewhere the binary stays in the build
# directory and the runtime files are linked beside it, because a directory symlink on Windows
# needs a privilege an ordinary build does not have.
if (WIN32)
    if (DIFFRACTOR_TARGET_ARCH STREQUAL "x64")
        set(_diffractor_stem diffractor64)
    elseif (DIFFRACTOR_TARGET_ARCH STREQUAL "x86")
        set(_diffractor_stem diffractor32)
    else ()
        set(_diffractor_stem diffractor-arm64)
    endif ()

    # The Store package ships one binary called diffractor.exe, whatever it was built from.
    if (DIFFRACTOR_WINSTORE AND NOT DIFFRACTOR_TARGET_ARCH STREQUAL "arm64")
        set(_diffractor_stem diffractor)
    endif ()

    set_target_properties(diffractor PROPERTIES
            OUTPUT_NAME "${_diffractor_stem}"
            OUTPUT_NAME_DEBUG "${_diffractor_stem}-d"
            PDB_NAME "${_diffractor_stem}"
            PDB_NAME_DEBUG "${_diffractor_stem}-d")

    # Stated per configuration as well as once, or a multi-config generator appends Debug/ or
    # Release/ to the directory and nothing downstream finds the binary.
    foreach (_dir_property RUNTIME_OUTPUT_DIRECTORY PDB_OUTPUT_DIRECTORY)
        set_target_properties(diffractor PROPERTIES
                ${_dir_property} "${CMAKE_SOURCE_DIR}/exe"
                ${_dir_property}_DEBUG "${CMAKE_SOURCE_DIR}/exe"
                ${_dir_property}_RELEASE "${CMAKE_SOURCE_DIR}/exe"
                ${_dir_property}_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/exe"
                ${_dir_property}_MINSIZEREL "${CMAKE_SOURCE_DIR}/exe")
    endforeach ()
else ()
    # Linked rather than copied: exe/test alone is around 100 MB, and a copy would go stale the
    # moment a fixture changed. Without this every fixture test fails on a missing file rather than
    # on its subject.
    foreach (_runtime_entry IN ITEMS
            test
            test-formats
            test-generated
            languages
            dictionaries
            location-countries.txt
            location-places.txt
            location-states.txt)
        set(_runtime_link "${CMAKE_CURRENT_BINARY_DIR}/${_runtime_entry}")

        if (NOT IS_SYMLINK "${_runtime_link}" AND NOT EXISTS "${_runtime_link}")
            file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/exe/${_runtime_entry}" "${_runtime_link}"
                    RESULT _runtime_link_result SYMBOLIC)

            if (NOT _runtime_link_result EQUAL 0)
                message(WARNING "Could not stage exe/${_runtime_entry}: ${_runtime_link_result}")
            endif ()
        endif ()
    endforeach ()
endif ()
