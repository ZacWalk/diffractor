# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Build upstream FFmpeg, fetched at the commit cmake/dd-dependencies.json pins, by driving its
# own configure and make, and expose the five libraries av_format.cpp uses as diffractor::ffmpeg.
# FFmpeg's build system is the only supported way to configure it; restating it in CMake would be a
# second source of truth for which codecs exist.

include_guard(GLOBAL)
include(DiffractorFetch)
include(ExternalProject)
include(ProcessorCount)

diffractor_fetch(ffmpeg)
set(_ff_src "${ffmpeg_SOURCE_DIR}")

# On Windows FFmpeg is not configured at all: what its configure generated for Windows - config-x64.h,
# config-x86.h, their nasm counterparts and the component lists - is kept in
# cmake/vendored/ffmpeg-windows for exactly this, because configure needs a shell. The source list
# lives in ffmpeg_msvc.cmake, with what it could not carry beside it in ffmpeg_msvc.local.cmake.
if (MSVC)
    include(vendored/ffmpeg_msvc OPTIONAL RESULT_VARIABLE _ff_msvc)

    if (TARGET diffractor_ffmpeg_msvc)
        add_library(diffractor::ffmpeg ALIAS diffractor_ffmpeg_msvc)
    else ()
        message(FATAL_ERROR "ffmpeg: cmake/vendored/ffmpeg_msvc.cmake is missing; media support cannot be built")
    endif ()

    return()
endif ()

# FFmpeg builds in a copy of the fetched tree, since this recipe builds in source, so the fetched tree
# stays free of build output.
set(_ff_stage "${CMAKE_BINARY_DIR}/third-party/ffmpeg/src")
set(_ff_prefix "${CMAKE_BINARY_DIR}/third-party/ffmpeg/install")

# Link order, most dependent first: these are static archives and the linker resolves in one pass.
set(_ff_libs avformat avcodec swscale swresample avutil)

foreach (lib IN LISTS _ff_libs)
    list(APPEND _ff_archives "${_ff_prefix}/lib/lib${lib}.a")
endforeach ()

ProcessorCount(_ff_jobs)

if (_ff_jobs EQUAL 0)
    set(_ff_jobs 1)
endif ()

# configure only accepts an external library through pkg-config, and the vendored copies install no
# .pc file, so describe the archives this build already produces. Without this for libopenmpt the
# demuxer is simply absent and a tracked module scans as nothing at all rather than failing loudly;
# without it for zlib, configure probes the system copy's headers while the link resolves against
# libfiles' zlib-ng beside it, which is a different implementation agreeing by convention.
#
# -L/-l rather than the archive's path: configure sorts its probe's arguments into compiler flags and
# libraries by looking for a -l prefix, so a bare path is placed ahead of the object it has to
# satisfy and ld, reading once, discards it.
set(_ff_pkgconfig "${CMAKE_BINARY_DIR}/third-party/ffmpeg/pkgconfig")

function(_ff_describe_vendored pc_name target version include_dir extra_libs)
    file(GENERATE OUTPUT "${_ff_pkgconfig}/${pc_name}.pc" CONTENT
            "Name: ${pc_name}\nDescription: Vendored ${pc_name}\nVersion: ${version}\nCflags: -I${include_dir}\nLibs: -L$<TARGET_FILE_DIR:${target}> -l${target} ${extra_libs}\n")
endfunction()

set(_ff_openmpt "")

if (TARGET diffractor_openmpt)
    set(_ff_openmpt --enable-libopenmpt)
    _ff_describe_vendored(libopenmpt diffractor_openmpt 0.8.9
            "${openmpt_SOURCE_DIR}" "-lstdc++ -lm")
endif ()

# libfiles' zlib-ng: its generated headers and the fetched tree, as -I for each.
if (TARGET libfiles-zlib)
    _ff_describe_vendored(zlib libfiles-zlib 1.3.1
            "$<JOIN:$<TARGET_PROPERTY:libfiles-zlib,INTERFACE_INCLUDE_DIRECTORIES>, -I>" "")
endif ()

# FFmpeg's own AV1 decoder only drives a hardware accelerator, so without libdav1d an AV1 file has
# no software decoder at all: no thumbnail, no preview, and no playback where the GPU cannot decode
# AV1. libfiles' dav1d is already linked for AVIF; this registers it with libavcodec too.
set(_ff_dav1d "")

if (TARGET libfiles-dav1d)
    set(_ff_dav1d --enable-libdav1d)
    _ff_describe_vendored(dav1d libfiles-dav1d 1.5.4
            "$<JOIN:$<TARGET_PROPERTY:libfiles-dav1d,INTERFACE_INCLUDE_DIRECTORIES>, -I>" "-lpthread -lm")
endif ()

# bzlib and lzma have no pkg-config path in configure at all -- it probes them with a hard coded
# -lbz2 and -llzma -- so the vendored archives are staged under the names it looks for. Naming the
# system copies instead would put a second bzip2 and a second lzma in a binary that already links
# the vendored ones.
set(_ff_libdir "${CMAKE_BINARY_DIR}/third-party/ffmpeg/lib")
set(_ff_compression "")

if (TARGET diffractor_bzip2 AND TARGET diffractor_lzma)
    set(_ff_compression
            --enable-bzlib
            --enable-lzma
            "--extra-cflags=-I$<JOIN:$<TARGET_PROPERTY:diffractor_bzip2,INTERFACE_INCLUDE_DIRECTORIES>, -I> -I${lzma_SOURCE_DIR}/src/liblzma/api"
            "--extra-ldflags=-L${_ff_libdir}")

    add_custom_command(
            OUTPUT "${_ff_libdir}/libbz2.a" "${_ff_libdir}/liblzma.a"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ff_libdir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:diffractor_bzip2>" "${_ff_libdir}/libbz2.a"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:diffractor_lzma>" "${_ff_libdir}/liblzma.a"
            DEPENDS diffractor_bzip2 diffractor_lzma
            COMMENT "Staging vendored bzip2 and lzma as libbz2.a and liblzma.a for FFmpeg's configure")

    add_custom_target(ffmpeg_compression_libs
            DEPENDS "${_ff_libdir}/libbz2.a" "${_ff_libdir}/liblzma.a")
endif ()

# FFmpeg builds static libraries and no shared ones unless told otherwise, and has had no libpostproc
# since 8.0, so none of those is named. --disable-autodetect keeps the result independent of whatever
# happens to be installed on the build machine, at the price of having to name everything wanted.
#
# zlib is named back because --disable-autodetect had silently taken it, and with it the thirty-odd
# decoders that depend on it -- APNG, EXR, TSCC, ZMBV and the screen-capture family are all present
# in the Windows config and were absent here.
#
# --disable-network to match the Windows config, which has never had it. Nothing in a local media
# organizer opens a socket, and it removes the RTP, RTSP, SAP and SDP demuxers from the attack
# surface along with the protocol layer beneath them.
#
# Diffractor is a broad-support reader: every decoder and demuxer is wanted, and nothing that writes
# a media stream is. The encoder, muxer, filter, device and protocol switches below are the Windows
# configure line from docs/third-party.md, which this had not been matching -- the whole encoder and
# muxer set was being compiled in here. file is the one intentional protocol, and there is no muxer.
#
# --enable-small is deliberately absent. It buys a smaller binary by stripping codec and profile
# names and by sending H.264 macroblocks, MPEG motion compensation and swscale output down their
# slow generic paths, and here it would also compile the whole library at -Os rather than -O3.
ExternalProject_Add(ffmpeg_external
        SOURCE_DIR "${_ff_stage}"
        DOWNLOAD_COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_ff_src}" "${_ff_stage}"
        CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${_ff_pkgconfig}"
        "${_ff_stage}/configure"
        --prefix=${_ff_prefix}
        --disable-programs
        --disable-doc
        --disable-avdevice
        --disable-avfilter
        --disable-autodetect
        --disable-network
        --disable-encoders
        --disable-muxers
        --disable-devices
        --disable-filters
        --disable-protocols
        --enable-protocol=file
        --enable-zlib
        ${_ff_compression}
        --enable-pic
        ${_ff_openmpt}
        ${_ff_dav1d}
        BUILD_COMMAND make -j${_ff_jobs}
        INSTALL_COMMAND make install
        BUILD_IN_SOURCE 1
        BUILD_BYPRODUCTS ${_ff_archives}
        LOG_DOWNLOAD ON
        LOG_CONFIGURE ON
        LOG_BUILD ON
        LOG_INSTALL ON
        USES_TERMINAL_BUILD OFF
)

# Restage when FFmpeg's own pin changes, and only then: file(CONFIGURE) rewrites the stamp only when
# its content differs, so another library's pin leaves this build alone.
file(READ "${CMAKE_SOURCE_DIR}/cmake/dd-dependencies.json" _ff_pins)
string(JSON _ff_pin GET "${_ff_pins}" dependencies ffmpeg)
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/third-party/ffmpeg/pin.json" CONTENT "${_ff_pin}" @ONLY)
ExternalProject_Add_StepDependencies(ffmpeg_external download
        "${CMAKE_BINARY_DIR}/third-party/ffmpeg/pin.json")

# The install tree is the only place libavutil/avconfig.h exists: it is generated by configure, so
# the source directory cannot stand in for it.
file(MAKE_DIRECTORY "${_ff_prefix}/include")

add_library(diffractor_ffmpeg INTERFACE)
target_include_directories(diffractor_ffmpeg INTERFACE "${_ff_prefix}/include")
target_link_libraries(diffractor_ffmpeg INTERFACE m ${CMAKE_DL_LIBS})

# The archives are handed to the application to place rather than carried on this target's link
# interface. A rescan group only covers the items written into it, and an interface target's own
# libraries are emitted after the group has closed -- which left libavformat behind libopenmpt on a
# line ld reads once, so its calls into the tracked module loader went unresolved.
set_property(GLOBAL PROPERTY DIFFRACTOR_GROUPED_ARCHIVES "${_ff_archives}")

# configure links a probe against the archives named in the .pc files, so they have to be on disk
# before the external project starts rather than merely before the application links.
foreach (_ff_dep IN ITEMS diffractor_openmpt libfiles-zlib libfiles-dav1d ffmpeg_compression_libs)
    if (TARGET ${_ff_dep})
        add_dependencies(ffmpeg_external ${_ff_dep})
    endif ()
endforeach ()

# An external project is not a CMake target on the link line, so the application has to be told to
# wait for it explicitly.
set_property(GLOBAL APPEND PROPERTY DIFFRACTOR_EXTERNAL_TARGETS ffmpeg_external)

add_library(diffractor::ffmpeg ALIAS diffractor_ffmpeg)
