# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Bring in lib8bit, the Commodore 64 emulator that plays SID tunes, at its pinned commit. A
# sibling checkout at ../lib8bit is used instead when one exists, so a lib8bit change can be built and
# tested here before it is committed there - but then this tree builds whatever that checkout holds,
# not the pin. CI and a fresh clone always build the pin.

include_guard(GLOBAL)
include(FetchContent)

set(DIFFRACTOR_LIB8BIT_REVISION db38a1b5c79e64e9f64ad0026c6dddac5ff31fc2)

if (NOT DEFINED FETCHCONTENT_SOURCE_DIR_LIB8BIT AND EXISTS "${CMAKE_SOURCE_DIR}/../lib8bit/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_LIB8BIT "${CMAKE_SOURCE_DIR}/../lib8bit" CACHE PATH
            "A lib8bit checkout to build instead of the pinned commit")
endif ()

# Brought in as a subproject, lib8bit builds only its emulator core: its Win32 test app and its suite
# are its own project's.
FetchContent_Declare(lib8bit
        GIT_REPOSITORY https://github.com/ZacWalk/lib8bit.git
        GIT_TAG ${DIFFRACTOR_LIB8BIT_REVISION})
FetchContent_MakeAvailable(lib8bit)

if (FETCHCONTENT_SOURCE_DIR_LIB8BIT)
    message(STATUS "lib8bit: local checkout ${FETCHCONTENT_SOURCE_DIR_LIB8BIT}")
else ()
    message(STATUS "lib8bit: pinned ${DIFFRACTOR_LIB8BIT_REVISION}")
endif ()
