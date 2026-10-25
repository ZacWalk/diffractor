# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Bring in a third-party library from its release archive, pinned by SHA-256 in
# cmake/dd-dependencies.json in dd's form - the pin, and nothing of the library vendored here. The
# archive is fetched once per build tree and not configured: Diffractor's own recipe in
# cmake/vendored/ builds it, from <name>_SOURCE_DIR.

include_guard(GLOBAL)
include(FetchContent)

function(diffractor_fetch NAME)
    file(READ "${CMAKE_SOURCE_DIR}/cmake/dd-dependencies.json" _pins)
    string(JSON _url ERROR_VARIABLE _missing GET "${_pins}" dependencies ${NAME} url)

    if (_missing)
        message(FATAL_ERROR "${NAME} has no pin in cmake/dd-dependencies.json")
    endif ()

    string(JSON _sha256 GET "${_pins}" dependencies ${NAME} sha256)

    # The recipes build the archive's sources themselves; upstream's own CMakeLists, where there is
    # one, is never configured.
    FetchContent_Declare(${NAME}
            URL "${_url}"
            URL_HASH SHA256=${_sha256}
            SOURCE_SUBDIR no-cmake-project
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(${NAME})
    set(${NAME}_SOURCE_DIR "${${NAME}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
