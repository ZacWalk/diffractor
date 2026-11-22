# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Bring in libmug, which finds faces and makes each a vector for face search (docs/faces.md), at its
# pinned commit. A sibling checkout at ../libmug is used instead when one exists, so a libmug change can be built
# and tested here before it is committed there - but then this tree builds whatever that checkout holds, not the
# pin. CI and a fresh clone always build the pin. libmug fetches libfacedetection's archive, whose weights it
# compiles in, as facedetection, so the face tests read its sample photographs from facedetection_SOURCE_DIR.

include_guard(GLOBAL)
include(FetchContent)

set(DIFFRACTOR_LIBMUG_REVISION fd94430daf52949ed719acaa4d7a8a072464178e)

if (NOT DEFINED FETCHCONTENT_SOURCE_DIR_LIBMUG AND EXISTS "${CMAKE_SOURCE_DIR}/../libmug/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_LIBMUG "${CMAKE_SOURCE_DIR}/../libmug" CACHE PATH
            "A libmug checkout to build instead of the pinned commit")
endif ()

# Its kernels on the Highway libfiles builds, so the application links one copy.
set(LIBMUG_HIGHWAY_TARGET libfiles::highway)
FetchContent_Declare(libmug
        GIT_REPOSITORY https://github.com/ZacWalk/libmug.git
        GIT_TAG ${DIFFRACTOR_LIBMUG_REVISION})
FetchContent_MakeAvailable(libmug)

if (FETCHCONTENT_SOURCE_DIR_LIBMUG)
    message(STATUS "libmug: local checkout ${FETCHCONTENT_SOURCE_DIR_LIBMUG}")
else ()
    message(STATUS "libmug: pinned ${DIFFRACTOR_LIBMUG_REVISION}")
endif ()
