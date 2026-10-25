# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: libheif's FFmpeg decoder plugin, built against Diffractor's FFmpeg from the libheif libfiles
# fetches, as diffractor::heif_ffmpeg, beside diffractor::heif, which is libfiles' libheif. libfiles
# builds libheif with its JPEG, libde265, dav1d and uncompressed decoders and leaves FFmpeg to the
# application, which has it; files_heif.cpp registers the plugin before libheif decodes anything. Its
# include directories and definitions are the ones the vendored libheif compiled it with. Both go on
# the link line as members of the archive group, so neither reaches the codecs from outside it.

include_guard(GLOBAL)
include(FetchContent)

FetchContent_GetProperties(libheif)

if (NOT libheif_POPULATED OR NOT TARGET libfiles-heif)
    message(FATAL_ERROR "libfiles did not bring libheif: build libfiles with LIBFILES_HEIF")
endif ()

# FFmpeg's headers, from the fork FFmpeg.cmake fetched.
FetchContent_GetProperties(ffmpeg)

if (NOT ffmpeg_POPULATED)
    message(FATAL_ERROR "FFmpeg was not fetched, and libheif's FFmpeg decoder plugin needs its headers")
endif ()

add_library(diffractor_heif_ffmpeg STATIC "${libheif_SOURCE_DIR}/libheif/plugins/decoder_ffmpeg.cc")
target_include_directories(diffractor_heif_ffmpeg PRIVATE
        "${libheif_SOURCE_DIR}/libheif"
        "${libheif_SOURCE_DIR}/libheif/plugins"
        "${ffmpeg_SOURCE_DIR}")
target_compile_definitions(diffractor_heif_ffmpeg PRIVATE
        HAVE_FFMPEG_DECODER ENABLE_MULTITHREADING_SUPPORT=1 ENABLE_PARALLEL_TILE_DECODING=1 IS_BIG_ENDIAN=0
        _CRT_NONSTDC_NO_DEPRECATE)

if (WIN32)
    target_compile_definitions(diffractor_heif_ffmpeg PRIVATE UNICODE _UNICODE)
endif ()

target_link_libraries(diffractor_heif_ffmpeg PUBLIC libfiles::heif)
diffractor_apply_vendored_policy(diffractor_heif_ffmpeg)

add_library(diffractor::heif ALIAS libfiles-heif)
add_library(diffractor::heif_ffmpeg ALIAS diffractor_heif_ffmpeg)
