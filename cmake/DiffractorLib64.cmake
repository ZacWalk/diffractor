# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Bring in lib64, the Commodore 64 emulator that plays SID tunes and runs disks, tapes,
# cartridges and programs, at its pinned commit. A sibling checkout at ../lib64 is used instead when
# one exists, so a lib64 change can be built and tested here before it is committed there - but then
# this tree builds whatever that checkout holds, not the pin. CI and a fresh clone always build the pin.

include_guard(GLOBAL)
include(FetchContent)

set(DIFFRACTOR_LIB64_REVISION b7cea35bc55d037232d7e23efd1adc4b4e4c4c10)

if (NOT DEFINED FETCHCONTENT_SOURCE_DIR_LIB64 AND EXISTS "${CMAKE_SOURCE_DIR}/../lib64/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_LIB64 "${CMAKE_SOURCE_DIR}/../lib64" CACHE PATH
            "A lib64 checkout to build instead of the pinned commit")
endif ()

# Brought in as a subproject, lib64 builds only its emulator core: its Win32 test app and its suite
# are its own project's.
FetchContent_Declare(lib64
        GIT_REPOSITORY https://github.com/ZacWalk/lib64.git
        GIT_TAG ${DIFFRACTOR_LIB64_REVISION})
FetchContent_MakeAvailable(lib64)

if (FETCHCONTENT_SOURCE_DIR_LIB64)
    message(STATUS "lib64: local checkout ${FETCHCONTENT_SOURCE_DIR_LIB64}")
else ()
    message(STATUS "lib64: pinned ${DIFFRACTOR_LIB64_REVISION}")
endif ()
