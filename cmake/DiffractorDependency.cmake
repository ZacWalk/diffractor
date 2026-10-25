# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Resolve one third-party dependency, either from a system package or from the release
# its owned module in cmake/vendored/ fetches, at the pin in cmake/dd-dependencies.json, and builds.
# The choice is per library because it is genuinely case by case -- see docs/third-party.md. FFmpeg
# is a Diffractor fork whose value is the patches it carries, so it is never offered a system option
# at all.

include_guard(GLOBAL)

find_package(PkgConfig QUIET)

# Static linking is the target state for a shipped build. It is not applied to system packages:
# distributions ship shared objects, static archives are inconsistently packaged, and linking
# them statically would move CVE patching from the distribution onto us.
option(DIFFRACTOR_STATIC_VENDORED "Build vendored third-party libraries as static archives" ON)

# A shipped Diffractor is fully vendored: docs/third-party.md records the patches and the pinned
# versions a distribution copy would silently discard, and the vendored zlib is zlib-ng rather than
# zlib at all. System packages remain available per library, as a way to bring a new platform up
# before its vendored modules build, not as the configuration anything ships in.
option(DIFFRACTOR_PREFER_SYSTEM "Default each dependency to the system copy where one is offered" OFF)

set(DIFFRACTOR_RESOLVED_SYSTEM "" CACHE INTERNAL "")
set(DIFFRACTOR_RESOLVED_VENDORED "" CACHE INTERNAL "")
set(DIFFRACTOR_RESOLVED_MISSING "" CACHE INTERNAL "")

# diffractor_dependency(<name>
#     PKG_CONFIG      <module>...     pkg-config module(s) for the system copy
#     VENDORED_MODULE <module>       owned cmake/ module that defines diffractor::<name>
#     [FORK]                         never resolvable from the system
#     [OPTIONAL]                     absence is reported, not fatal
# )
#
# Defines the imported target diffractor::<name> when the dependency resolves.
function(diffractor_dependency NAME)
    cmake_parse_arguments(DEP "FORK;OPTIONAL" "VENDORED_MODULE" "PKG_CONFIG" ${ARGN})

    string(TOUPPER "${NAME}" UPPER)
    set(TARGET_NAME "diffractor::${NAME}")

    if (DEP_FORK)
        # No option is declared: a system copy of a fork silently discards the patches that are
        # the entire reason it is vendored.
        set(USE_SYSTEM OFF)
    else ()
        option(DIFFRACTOR_SYSTEM_${UPPER}
                "Use the system ${NAME} rather than the pinned release" ${DIFFRACTOR_PREFER_SYSTEM})
        set(USE_SYSTEM ${DIFFRACTOR_SYSTEM_${UPPER}})
    endif ()

    if (USE_SYSTEM AND DEP_PKG_CONFIG AND PKG_CONFIG_FOUND)
        pkg_check_modules(PC_${UPPER} QUIET IMPORTED_TARGET ${DEP_PKG_CONFIG})

        if (PC_${UPPER}_FOUND)
            add_library(${TARGET_NAME} INTERFACE IMPORTED GLOBAL)
            target_link_libraries(${TARGET_NAME} INTERFACE PkgConfig::PC_${UPPER})
            set(DIFFRACTOR_RESOLVED_SYSTEM
                    "${DIFFRACTOR_RESOLVED_SYSTEM};${NAME} ${PC_${UPPER}_VERSION}" CACHE INTERNAL "")
            return()
        endif ()
    endif ()

    # An owned module fetches the library at its pin and builds it, restating its sources or, for
    # FFmpeg on Linux, driving its own build system.
    if (DEP_VENDORED_MODULE)
        include(${DEP_VENDORED_MODULE})

        if (TARGET ${TARGET_NAME})
            set(DIFFRACTOR_RESOLVED_VENDORED
                    "${DIFFRACTOR_RESOLVED_VENDORED};${NAME}" CACHE INTERNAL "")
            return()
        endif ()
    endif ()

    set(DIFFRACTOR_RESOLVED_MISSING "${DIFFRACTOR_RESOLVED_MISSING};${NAME}" CACHE INTERNAL "")

    if (NOT DEP_OPTIONAL)
        if (DEP_FORK)
            message(FATAL_ERROR
                    "Fork '${NAME}' did not resolve, and it has no system or stubbed alternative.\n"
                    "  module: cmake/${DEP_VENDORED_MODULE}.cmake")
        endif ()

        message(FATAL_ERROR
                "Dependency '${NAME}' did not resolve.\n"
                "  system:   ${DEP_PKG_CONFIG} (pkg-config)\n"
                "  module:   cmake/${DEP_VENDORED_MODULE}.cmake\n"
                "Run './dd setup' to install the system packages.")
    endif ()
endfunction()

function(diffractor_report_dependencies)
    message(STATUS "")
    message(STATUS "Dependencies")

    foreach (d IN LISTS DIFFRACTOR_RESOLVED_SYSTEM)
        if (d)
            message(STATUS "  system    ${d}")
        endif ()
    endforeach ()

    foreach (d IN LISTS DIFFRACTOR_RESOLVED_VENDORED)
        if (d)
            message(STATUS "  vendored  ${d}")
        endif ()
    endforeach ()

    foreach (d IN LISTS DIFFRACTOR_RESOLVED_MISSING)
        if (d)
            message(STATUS "  MISSING   ${d}")
        endif ()
    endforeach ()

    message(STATUS "")
endfunction()
