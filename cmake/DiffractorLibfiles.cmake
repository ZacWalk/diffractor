# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Bring in libfiles, the file-format library Diffractor shares with ImageWalker, at its
# pinned commit. A sibling checkout at ../libfiles is used instead when one exists, so a libfiles
# change can be built and tested here before it is committed there - but then this tree builds
# whatever that checkout holds, not the pin. CI and a fresh clone always build the pin.

include_guard(GLOBAL)
include(FetchContent)

set(DIFFRACTOR_LIBFILES_REVISION a0655f623693a663ffad0a59ff11acc16da389cd)

if (NOT DEFINED FETCHCONTENT_SOURCE_DIR_LIBFILES AND EXISTS "${CMAKE_SOURCE_DIR}/../libfiles/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_LIBFILES "${CMAKE_SOURCE_DIR}/../libfiles" CACHE PATH
            "A libfiles checkout to build instead of the pinned commit")
endif ()

# GIF and TIFF pixel decoding remains FFmpeg's here; native document storage is ImageWalker's.
set(LIBFILES_GIF OFF)
set(LIBFILES_TIFF OFF)
FetchContent_Declare(libfiles
        GIT_REPOSITORY https://github.com/ZacWalk/libfiles.git
        GIT_TAG ${DIFFRACTOR_LIBFILES_REVISION})
FetchContent_MakeAvailable(libfiles)

if (FETCHCONTENT_SOURCE_DIR_LIBFILES)
    message(STATUS "libfiles: local checkout ${FETCHCONTENT_SOURCE_DIR_LIBFILES}")
else ()
    message(STATUS "libfiles: pinned ${DIFFRACTOR_LIBFILES_REVISION}")
endif ()
