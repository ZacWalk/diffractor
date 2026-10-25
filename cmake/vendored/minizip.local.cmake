# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: What the minizip .vcxproj could not describe. minizip implements its OS layer, its
# stream and its crypto once per platform, and the project names only the Win32 ones. The crypto
# backend is OpenSSL here, which is what minizip itself offers for a generic POSIX build.

if (NOT WIN32)
    foreach (windows_only mz_os_win32.c mz_strm_os_win32.c mz_crypt_winvista.c)
        set_source_files_properties("${minizip_SOURCE_DIR}/${windows_only}"
                PROPERTIES HEADER_FILE_ONLY ON)
    endforeach ()

    target_sources(diffractor_minizip PRIVATE
            "${minizip_SOURCE_DIR}/mz_os_posix.c"
            "${minizip_SOURCE_DIR}/mz_strm_os_posix.c"
            "${minizip_SOURCE_DIR}/mz_crypt_openssl.c")

    target_link_libraries(diffractor_minizip PRIVATE crypto)
endif ()
