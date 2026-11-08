# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Bring in libdesktop's core, platform, render and UI libraries - the foundation (core::), the
# OS services (platform::), the drawing contexts (render::) and the desktop UI (ui::) Diffractor is written
# in, with the syntax highlighters the panel's document colours text with - at its pinned commit. A sibling
# checkout at ../libdesktop is used instead when one exists, so a libdesktop change can be built and tested
# here before it is committed there - but then this tree builds whatever that checkout holds, not the pin.
# CI and a fresh clone always build the pin.

include_guard(GLOBAL)
include(FetchContent)

set(DIFFRACTOR_LIBDESKTOP_REVISION 04109ee82a500db38d439fa281bbc3f43124adfe)

if (NOT DEFINED FETCHCONTENT_SOURCE_DIR_LIBDESKTOP AND EXISTS "${CMAKE_SOURCE_DIR}/../libdesktop/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_LIBDESKTOP "${CMAKE_SOURCE_DIR}/../libdesktop" CACHE PATH
            "A libdesktop checkout to build instead of the pinned commit")
endif ()

# RapidJSON comes with libdesktop, which pins its archive. Trees configured while a copy of it was
# vendored here still name that copy, which is gone.
if (FETCHCONTENT_SOURCE_DIR_RAPIDJSON STREQUAL "${CMAKE_SOURCE_DIR}/third-party/rapidjson")
    unset(FETCHCONTENT_SOURCE_DIR_RAPIDJSON CACHE)
endif ()

# A Store build builds libdesktop's platform library for the Store package too, which defines WINSTORE.
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
