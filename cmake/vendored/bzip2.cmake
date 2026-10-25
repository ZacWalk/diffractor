# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Build bzip2, fetched at its pinned release, as diffractor::bzip2.
#
# Imported from the bzip2.vcxproj of the copy once vendored here, by tools/import_vcxproj.py. The
# release builds unchanged: the copy spelled four POSIX names the MSVC way, and the vendored policy's
# _CRT_NONSTDC_NO_WARNINGS takes the release's own spellings. Compiler and linker flags are not
# restated here: DiffractorCompilerPolicy.cmake owns those for every target.

include_guard(GLOBAL)
include(DiffractorFetch)
diffractor_fetch(bzip2)

add_library(diffractor_bzip2 STATIC
        "${bzip2_SOURCE_DIR}/blocksort.c"
        "${bzip2_SOURCE_DIR}/bzip2.c"
        "${bzip2_SOURCE_DIR}/bzlib.c"
        "${bzip2_SOURCE_DIR}/compress.c"
        "${bzip2_SOURCE_DIR}/crctable.c"
        "${bzip2_SOURCE_DIR}/decompress.c"
        "${bzip2_SOURCE_DIR}/huffman.c"
        "${bzip2_SOURCE_DIR}/randtable.c"
)

target_include_directories(diffractor_bzip2 PUBLIC
        "${bzip2_SOURCE_DIR}"
)

if (WIN32)
    target_compile_definitions(diffractor_bzip2 PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:UNICODE>" "$<$<COMPILE_LANGUAGE:C,CXX>:_UNICODE>")
endif ()

diffractor_apply_vendored_policy(diffractor_bzip2)

add_library(diffractor::bzip2 ALIAS diffractor_bzip2)
