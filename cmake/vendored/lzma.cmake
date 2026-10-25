# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Build liblzma, fetched at its pinned XZ release by CMakeLists.txt, as diffractor::lzma.
#
# Imported from the liblzma.vcxproj of the copy once vendored here, by tools/import_vcxproj.py. The
# release's sources build unchanged, with the config.h its configure produced for Windows, kept beside
# this file. Compiler and linker flags are not restated here: DiffractorCompilerPolicy.cmake owns those
# for every target.

include_guard(GLOBAL)

add_library(diffractor_lzma STATIC
        "${lzma_SOURCE_DIR}/src/common/tuklib_cpucores.c"
        "${lzma_SOURCE_DIR}/src/common/tuklib_exit.c"
        "${lzma_SOURCE_DIR}/src/common/tuklib_mbstr_fw.c"
        "${lzma_SOURCE_DIR}/src/common/tuklib_mbstr_width.c"
        "${lzma_SOURCE_DIR}/src/common/tuklib_open_stdxxx.c"
        "${lzma_SOURCE_DIR}/src/common/tuklib_physmem.c"
        "${lzma_SOURCE_DIR}/src/common/tuklib_progname.c"
        "${lzma_SOURCE_DIR}/src/liblzma/check/check.c"
        "${lzma_SOURCE_DIR}/src/liblzma/check/crc32_fast.c"
        "${lzma_SOURCE_DIR}/src/liblzma/check/crc64_fast.c"
        "${lzma_SOURCE_DIR}/src/liblzma/check/sha256.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/alone_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/alone_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/auto_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/block_buffer_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/block_buffer_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/block_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/block_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/block_header_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/block_header_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/block_util.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/common.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/easy_buffer_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/easy_decoder_memusage.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/easy_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/easy_encoder_memusage.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/easy_preset.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/filter_buffer_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/filter_buffer_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/filter_common.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/filter_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/filter_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/filter_flags_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/filter_flags_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/hardware_cputhreads.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/hardware_physmem.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/index.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/index_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/index_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/index_hash.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/outqueue.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/stream_buffer_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/stream_buffer_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/stream_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/stream_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/stream_encoder_mt.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/stream_flags_common.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/stream_flags_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/stream_flags_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/vli_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/vli_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/common/vli_size.c"
        "${lzma_SOURCE_DIR}/src/liblzma/delta/delta_common.c"
        "${lzma_SOURCE_DIR}/src/liblzma/delta/delta_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/delta/delta_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/lzma/fastpos_table.c"
        "${lzma_SOURCE_DIR}/src/liblzma/lzma/lzma2_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/lzma/lzma2_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/lzma/lzma_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/lzma/lzma_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/lzma/lzma_encoder_optimum_fast.c"
        "${lzma_SOURCE_DIR}/src/liblzma/lzma/lzma_encoder_optimum_normal.c"
        "${lzma_SOURCE_DIR}/src/liblzma/lzma/lzma_encoder_presets.c"
        "${lzma_SOURCE_DIR}/src/liblzma/lz/lz_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/lz/lz_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/lz/lz_encoder_mf.c"
        "${lzma_SOURCE_DIR}/src/liblzma/rangecoder/price_table.c"
        "${lzma_SOURCE_DIR}/src/liblzma/simple/arm.c"
        "${lzma_SOURCE_DIR}/src/liblzma/simple/armthumb.c"
        "${lzma_SOURCE_DIR}/src/liblzma/simple/ia64.c"
        "${lzma_SOURCE_DIR}/src/liblzma/simple/powerpc.c"
        "${lzma_SOURCE_DIR}/src/liblzma/simple/simple_coder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/simple/simple_decoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/simple/simple_encoder.c"
        "${lzma_SOURCE_DIR}/src/liblzma/simple/sparc.c"
        "${lzma_SOURCE_DIR}/src/liblzma/simple/x86.c"
)

target_include_directories(diffractor_lzma PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/lzma"
        "${lzma_SOURCE_DIR}/src/liblzma/common"
        "${lzma_SOURCE_DIR}/src/common"
        "${lzma_SOURCE_DIR}/src/liblzma/api"
        "${lzma_SOURCE_DIR}/src/liblzma/check"
        "${lzma_SOURCE_DIR}/src/liblzma/delta"
        "${lzma_SOURCE_DIR}/src/liblzma/lz"
        "${lzma_SOURCE_DIR}/src/liblzma/lzma"
        "${lzma_SOURCE_DIR}/src/liblzma/rangecoder"
        "${lzma_SOURCE_DIR}/src/liblzma/simple"
)

target_compile_definitions(diffractor_lzma PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:HAVE_CONFIG_H>" "$<$<COMPILE_LANGUAGE:C,CXX>:LZMA_API_STATIC>")

if (WIN32)
    target_compile_definitions(diffractor_lzma PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:UNICODE>" "$<$<COMPILE_LANGUAGE:C,CXX>:_UNICODE>")
endif ()

diffractor_apply_vendored_policy(diffractor_lzma)

# Anything the Windows project could not describe - a header its build generates, a flag a
# different compiler needs. Hand written, and kept out of this file so that re-importing
# does not discard it.
include("${CMAKE_CURRENT_LIST_DIR}/lzma.local.cmake" OPTIONAL)

add_library(diffractor::lzma ALIAS diffractor_lzma)
