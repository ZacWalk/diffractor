# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: What ffmpeg.vcxproj could express and the import could not. The project is a union
# superset serving x64, Win32 and ARM64, and it resolves the differences with per-platform
# ExcludedFromBuild on individual assembly sources. tools/import_vcxproj.py read one configuration
# (Release|x64), so the generated module carries one architecture's answer.
#
# These four are x86-64 only, and on Win32 they fail with "invalid operands in non-64-bit mode"
# rather than assembling to nothing the way the ARCH-guarded sources do. The list is transcribed
# from the project rather than derived from the errors, so a source that starts failing for some
# other reason is not silently dropped here.
#
# HEADER_FILE_ONLY is CMake's ExcludedFromBuild: the source stays on the target and is not built.
# This file is included after add_library, so removing them from a list would be too late.

if (DIFFRACTOR_TARGET_ARCH STREQUAL "x86")
    set_source_files_properties(
            "${ffmpeg_SOURCE_DIR}/libavcodec/x86/simple_idct10.asm"
            "${ffmpeg_SOURCE_DIR}/libswscale/x86/ops_common.asm"
            "${ffmpeg_SOURCE_DIR}/libswscale/x86/ops_float.asm"
            "${ffmpeg_SOURCE_DIR}/libswscale/x86/ops_int.asm"
            PROPERTIES HEADER_FILE_ONLY ON)

    # PREFIX makes x86inc.asm emit the leading underscore that MSVC gives every 32-bit cdecl C
    # symbol. The project sets it on the Win32 nasm item only, so importing the x64 configuration
    # could not see it, and without it every SIMD entry point assembles fine and is unresolved at
    # link -- the object exports ff_yuv_420_rgb16_ssse3 while the C side asks for
    # _ff_yuv_420_rgb16_ssse3.
    target_compile_definitions(diffractor_ffmpeg_msvc PRIVATE
            $<$<COMPILE_LANGUAGE:ASM_NASM>:PREFIX>)
endif ()
