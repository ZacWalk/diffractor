# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Build minizip-ng, fetched at its pinned release, as diffractor::minizip.
#
# Imported from the minizip.vcxproj of the copy once vendored here, by tools/import_vcxproj.py. The
# release builds unchanged, with the mz_config.h generated for Windows kept beside this file.
# Compiler and linker flags are not restated here: DiffractorCompilerPolicy.cmake owns those
# for every target.

include_guard(GLOBAL)
include(DiffractorFetch)
diffractor_fetch(minizip)

add_library(diffractor_minizip STATIC
        "${minizip_SOURCE_DIR}/compat/ioapi.c"
        "${minizip_SOURCE_DIR}/compat/unzip.c"
        "${minizip_SOURCE_DIR}/compat/zip.c"
        "${minizip_SOURCE_DIR}/mz_crypt.c"
        "${minizip_SOURCE_DIR}/mz_crypt_winvista.c"
        "${minizip_SOURCE_DIR}/mz_os.c"
        "${minizip_SOURCE_DIR}/mz_os_win32.c"
        "${minizip_SOURCE_DIR}/mz_strm.c"
        "${minizip_SOURCE_DIR}/mz_strm_buf.c"
        "${minizip_SOURCE_DIR}/mz_strm_bzip.c"
        "${minizip_SOURCE_DIR}/mz_strm_lzma.c"
        "${minizip_SOURCE_DIR}/mz_strm_mem.c"
        "${minizip_SOURCE_DIR}/mz_strm_os_win32.c"
        "${minizip_SOURCE_DIR}/mz_strm_pkcrypt.c"
        "${minizip_SOURCE_DIR}/mz_strm_split.c"
        "${minizip_SOURCE_DIR}/mz_strm_wzaes.c"
        "${minizip_SOURCE_DIR}/mz_strm_zlib.c"
        "${minizip_SOURCE_DIR}/mz_zip.c"
        "${minizip_SOURCE_DIR}/mz_zip_rw.c"
)

target_include_directories(diffractor_minizip PUBLIC
        "${minizip_SOURCE_DIR}"
        "${CMAKE_CURRENT_LIST_DIR}/minizip"
        "$<TARGET_PROPERTY:diffractor_bzip2,INTERFACE_INCLUDE_DIRECTORIES>"
        "${lzma_SOURCE_DIR}/src/liblzma/api"
)

# zlib comes with libfiles, and with it its headers.
target_link_libraries(diffractor_minizip PUBLIC diffractor::zlib)

target_compile_definitions(diffractor_minizip PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:ZLIB_COMPAT>" "$<$<COMPILE_LANGUAGE:C,CXX>:LZMA_API_STATIC>" "$<$<COMPILE_LANGUAGE:C,CXX>:HAVE_BZIP2>" "$<$<COMPILE_LANGUAGE:C,CXX>:HAVE_ZLIB>" "$<$<COMPILE_LANGUAGE:C,CXX>:HAVE_LZMA>" "$<$<COMPILE_LANGUAGE:C,CXX>:_CRT_NONSTDC_NO_DEPRECATE>" "$<$<COMPILE_LANGUAGE:C,CXX>:WIN64>")

if (WIN32)
    target_compile_definitions(diffractor_minizip PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:UNICODE>" "$<$<COMPILE_LANGUAGE:C,CXX>:_UNICODE>")
endif ()

diffractor_apply_vendored_policy(diffractor_minizip)

# Anything the Windows project could not describe - a header its build generates, a flag a
# different compiler needs. Hand written, and kept out of this file so that re-importing
# does not discard it.
include("${CMAKE_CURRENT_LIST_DIR}/minizip.local.cmake" OPTIONAL)

add_library(diffractor::minizip ALIAS diffractor_minizip)
