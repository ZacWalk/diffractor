# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Bring in libdesktop's system and render modules - the foundation (df::, str::), the OS
# services (platform::) and the desktop UI (ui::) Diffractor is written in - at its pinned commit. A sibling checkout at ../libdesktop
# is used instead when one exists, so a libdesktop change can be built and tested here before it is
# committed there - but then this tree builds whatever that checkout holds, not the pin. CI and a
# fresh clone always build the pin.

include_guard(GLOBAL)
include(FetchContent)

set(DIFFRACTOR_LIBDESKTOP_REVISION 184bdd0648a25b8dd225c332960ed967edb22043)

if (NOT DEFINED FETCHCONTENT_SOURCE_DIR_LIBDESKTOP AND EXISTS "${CMAKE_SOURCE_DIR}/../libdesktop/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_LIBDESKTOP "${CMAKE_SOURCE_DIR}/../libdesktop" CACHE PATH
            "A libdesktop checkout to build instead of the pinned commit")
endif ()

# RapidJSON comes with libdesktop, which pins its archive. Trees configured while a copy of it was
# vendored here still name that copy, which is gone.
if (FETCHCONTENT_SOURCE_DIR_RAPIDJSON STREQUAL "${CMAKE_SOURCE_DIR}/third-party/rapidjson")
    unset(FETCHCONTENT_SOURCE_DIR_RAPIDJSON CACHE)
endif ()

# The system and render modules only: libdesktop-core's window model is not Diffractor's, so nothing
# else in libdesktop is built.
set(LIBDESKTOP_SYSTEM ON)
set(LIBDESKTOP_RENDER ON)
set(LIBDESKTOP_STORE_PACKAGE ${DIFFRACTOR_WINSTORE})

FetchContent_Declare(libdesktop
        GIT_REPOSITORY https://github.com/ZacWalk/libdesktop.git
        GIT_TAG ${DIFFRACTOR_LIBDESKTOP_REVISION}
        EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(libdesktop)

if (FETCHCONTENT_SOURCE_DIR_LIBDESKTOP)
    message(STATUS "libdesktop: local checkout ${FETCHCONTENT_SOURCE_DIR_LIBDESKTOP}")
else ()
    message(STATUS "libdesktop: pinned ${DIFFRACTOR_LIBDESKTOP_REVISION}")
endif ()
