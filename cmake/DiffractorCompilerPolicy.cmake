# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: The compiler and linker policy for every target, application and vendored alike. It is
# stated once here rather than per target so that "are the flags right" is one file to review.
#
# The MSVC half began as a transcription of src/app.vcxproj, annotated with the project setting
# each flag came from. That project is deleted, so this file is now the only record of it: nothing
# here is guesswork, anything that could not be traced to a setting was left out rather than
# invented, and what was traced and deliberately dropped is in tools/build-divergence.txt.

include_guard(GLOBAL)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)

if (MSVC)
    # RuntimeLibrary: MultiThreaded / MultiThreadedDebug. CMake defaults to the DLL runtime, which
    # would change what the shipped binary depends on without changing anything a test observes.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Guarded by language like the switches below. nasm sees these too otherwise, and libjpeg's
    # jsimdext.inc reads WIN32 as "this is a 32-bit build" and decorates every SIMD symbol with a
    # leading underscore that nothing then resolves.
    add_compile_definitions($<$<COMPILE_LANGUAGE:C,CXX>:WIN32> $<$<COMPILE_LANGUAGE:C,CXX>:_WINDOWS>)

    # Guarded by language throughout: these are cl.exe switches, and nasm assembles some of the
    # vendored SIMD. An assembler handed /bigobj does not warn, it fails.
    add_compile_options(
            $<$<COMPILE_LANGUAGE:C,CXX>:/bigobj>     # AdditionalOptions
            $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>      # source and execution charset, not a warning switch
            $<$<COMPILE_LANGUAGE:C,CXX>:/FS>         # required for /MP to share one PDB writer
            $<$<COMPILE_LANGUAGE:C,CXX>:/GF>         # StringPooling
            $<$<COMPILE_LANGUAGE:C,CXX>:/Gy>         # FunctionLevelLinking
            $<$<COMPILE_LANGUAGE:C,CXX>:/Oi>         # IntrinsicFunctions
            $<$<COMPILE_LANGUAGE:C,CXX>:/GS>         # BufferSecurityCheck, on in both configurations
            $<$<COMPILE_LANGUAGE:C,CXX>:/MP>         # MultiProcessorCompilation
            $<$<COMPILE_LANGUAGE:C,CXX>:/Zi>         # DebugInformationFormat, in Release too
            $<$<COMPILE_LANGUAGE:CXX>:/EHsc>         # ExceptionHandling Sync
            $<$<COMPILE_LANGUAGE:CXX>:/GR>           # RuntimeTypeInfo
            $<$<COMPILE_LANGUAGE:C,CXX>:/fp:precise> # preserve floating-point semantics
                $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/O2>
                $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/GL>
    )

    # EnableEnhancedInstructionSet StreamingSIMDExtensions2. SSE2 is unconditional on x64, where
    # the switch is rejected rather than ignored, so it is only meaningful for the 32-bit build.
    # Language-guarded like the switches above: nasm reads /arch:SSE2 as a second input file and
    # fails with "more than one input file specified".
    #
    # This is the whole reason platform::has_avx2 exists: the baseline is SSE2 and anything above it
    # is dispatched at run time. Raising it here would pass every test on every machine that runs
    # tests and fault on the machines least able to say why.
    if (DIFFRACTOR_TARGET_ARCH STREQUAL "x86")
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/arch:SSE2>)
    endif ()

    # Optimization, InlineFunctionExpansion, BasicRuntimeChecks, ControlFlowGuard. Guard is on in
    # Debug and off in Release, which is the reverse of the usual arrangement; it is transcribed as
    # found rather than corrected here.
    set(CMAKE_C_FLAGS_DEBUG "/Od /Ob0 /RTC1 /guard:cf /D_DEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")

    # Optimization MaxSpeed, InlineFunctionExpansion AnySuitable, FavorSizeOrSpeed Size,
    # OmitFramePointers, WholeProgramOptimization, SupportJustMyCode false.
    set(CMAKE_C_FLAGS_RELEASE "/Ob2 /Os /Oy /JMC- /DNDEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")

    # GenerateDebugInformation is true in both configurations: a release build without symbols
    # cannot be attributed to a cause when it crashes. See docs/crash.md.
    add_link_options(/DEBUG)

    # OptimizeReferences, EnableCOMDATFolding, LinkTimeCodeGeneration
    # UseFastLinkTimeCodeGeneration. Spelled out rather than using CMake's interprocedural
    # optimisation property, which emits plain /LTCG and would not be the same build. SetChecksum
    # is the /RELEASE flag, and it writes the PE checksum an installer can verify.
    foreach (kind EXE SHARED MODULE)
        set(CMAKE_${kind}_LINKER_FLAGS_RELEASE "/OPT:REF /OPT:ICF /LTCG:incremental /RELEASE")
    endforeach ()
else ()
    add_compile_options(
            -Wall
            -Wno-unknown-pragmas
            -Wno-multichar
            # GCC 13 makes this an error by default. Accessors such as ui::surface::orientation()
            # share a name with a type in the enclosing namespace, which is ill-formed by the letter
            # of the standard and accepted by every compiler; renaming them buys nothing. C++ only:
            # the C compiler rejects the option rather than ignoring it.
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-changes-meaning>
    )
endif ()

# Diffractor's own code. Warnings are on, and the precompiled header is applied where it works.
function(diffractor_apply_app_policy target)
    if (MSVC)
        # WarningLevel Level3 and SDLCheck. Both judge source we own and can fix, so neither is
        # applied globally: /sdl promotes the deprecated-CRT warning to an error, which no vendored
        # C library written before those functions were deprecated would survive.
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/W3> $<$<COMPILE_LANGUAGE:C,CXX>:/sdl>)

        # CharacterSet Unicode. This is what makes RegCreateKeyEx and its like resolve to the wide
        # entry points the code passes wchar_t strings to.
        #
        # Two defines in the project are deliberately not carried: STRICT and
        # _WINSOCK_DEPRECATED_NO_WARNINGS appear only in the Win32 configurations, and WIN32 itself
        # appears everywhere except Win32 Release. That is drift between configurations of one
        # product, not a 32-bit requirement. What is stated here is what the shipped x64 binary is
        # built with, uniformly, which is the point of having one description.
        target_compile_definitions(${target} PRIVATE UNICODE _UNICODE)

        # The Store configuration was Release plus this one define, and it disables the
        # application-owned update path because the Store performs updates itself.
        if (DIFFRACTOR_WINSTORE)
            target_compile_definitions(${target} PRIVATE WINSTORE)
        endif ()

        # PrecompiledHeader Use, pch.h. Not applied elsewhere: CMake's forced include reaches the
        # same header by a different path, which defeats #pragma once on a case-preserving
        # case-insensitive filesystem and reports every declaration in it as a redefinition.
        target_precompile_headers(${target} PRIVATE "${CMAKE_SOURCE_DIR}/src/pch.h")

        # SubSystem Windows. The entry point is libdesktop-render's wWinMain, which lives in a library,
        # where the linker does not look when it picks the CRT's startup: named, the startup pulls it in.
        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE ON)
        target_link_options(${target} PRIVATE /ENTRY:wWinMainCRTStartup)

        # GenerateManifest false. platform_win_res.rc embeds platform_win.manifest itself, and a
        # linker-generated one collides with it rather than being ignored.
        target_link_options(${target} PRIVATE /MANIFEST:NO)
    endif ()
endfunction()

# Vendored code. It is not ours to make warning-clean, and the noise would bury our own.
function(diffractor_apply_vendored_policy target)
    if (MSVC)
        # These libraries are C of an age that predates the secure CRT, and the .vcxproj files all
        # said so themselves.
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/W0>)
        target_compile_definitions(${target} PRIVATE
                $<$<COMPILE_LANGUAGE:C,CXX>:_CRT_SECURE_NO_WARNINGS>
                $<$<COMPILE_LANGUAGE:C,CXX>:_CRT_NONSTDC_NO_WARNINGS>)
    else ()
        # Must not reach an assembler: there -w expects an argument and would swallow the next
        # define.
        target_compile_options(${target} PRIVATE
                $<$<COMPILE_LANGUAGE:C>:-w>
                $<$<COMPILE_LANGUAGE:CXX>:-w>)
    endif ()

    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()
