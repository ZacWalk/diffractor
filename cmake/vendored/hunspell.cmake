# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Build Hunspell, fetched at its pinned release, as diffractor::hunspell.
#
# Imported from the hunspell.vcxproj of the copy once vendored here, by tools/import_vcxproj.py. The
# release's src/hunspell/ builds unchanged.
# Compiler and linker flags are not restated here: DiffractorCompilerPolicy.cmake owns those
# for every target.

include_guard(GLOBAL)
include(DiffractorFetch)
diffractor_fetch(hunspell)

add_library(diffractor_hunspell STATIC
        "${hunspell_SOURCE_DIR}/src/hunspell/affentry.cxx"
        "${hunspell_SOURCE_DIR}/src/hunspell/affixmgr.cxx"
        "${hunspell_SOURCE_DIR}/src/hunspell/csutil.cxx"
        "${hunspell_SOURCE_DIR}/src/hunspell/filemgr.cxx"
        "${hunspell_SOURCE_DIR}/src/hunspell/hashmgr.cxx"
        "${hunspell_SOURCE_DIR}/src/hunspell/hunspell.cxx"
        "${hunspell_SOURCE_DIR}/src/hunspell/hunspelltrace.cxx"
        "${hunspell_SOURCE_DIR}/src/hunspell/hunzip.cxx"
        "${hunspell_SOURCE_DIR}/src/hunspell/phonet.cxx"
        "${hunspell_SOURCE_DIR}/src/hunspell/replist.cxx"
        "${hunspell_SOURCE_DIR}/src/hunspell/suggestmgr.cxx"
)

target_include_directories(diffractor_hunspell PUBLIC
        "${hunspell_SOURCE_DIR}/src/hunspell"
)

target_compile_definitions(diffractor_hunspell PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:W32>" "$<$<COMPILE_LANGUAGE:C,CXX>:HUNSPELL_STATIC>")

if (WIN32)
    target_compile_definitions(diffractor_hunspell PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:UNICODE>" "$<$<COMPILE_LANGUAGE:C,CXX>:_UNICODE>")
endif ()

diffractor_apply_vendored_policy(diffractor_hunspell)

# Anything the Windows project could not describe - a header its build generates, a flag a
# different compiler needs. Hand written, and kept out of this file so that re-importing
# does not discard it.
include("${CMAKE_CURRENT_LIST_DIR}/hunspell.local.cmake" OPTIONAL)

add_library(diffractor::hunspell ALIAS diffractor_hunspell)
