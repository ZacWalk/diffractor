# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Build libarchive, fetched at its pinned release, as diffractor::archive.
#
# Imported from the libarchive.vcxproj of the copy once vendored here, by tools/import_vcxproj.py.
# The release builds unchanged; the configuration its configure produced for each platform is kept
# here, in archive-windows/ and archive-linux/.
# Compiler and linker flags are not restated here: DiffractorCompilerPolicy.cmake owns those
# for every target.

include_guard(GLOBAL)
include(DiffractorFetch)
diffractor_fetch(libarchive)

add_library(diffractor_archive STATIC
        "${libarchive_SOURCE_DIR}/libarchive/archive_acl.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_blake2sp_ref.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_blake2s_ref.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_check_magic.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_cmdline.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_cryptor.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_digest.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_disk_acl_darwin.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_disk_acl_freebsd.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_disk_acl_linux.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_disk_acl_sunos.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_entry.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_entry_copy_bhfi.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_entry_copy_stat.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_entry_link_resolver.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_entry_sparse.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_entry_stat.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_entry_strmode.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_entry_xattr.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_parse_date.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_hmac.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_match.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_options.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_pack_dev.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_pathmatch.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_ppmd7.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_ppmd8.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_random.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_rb.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_add_passphrase.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_append_filter.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_data_into_fd.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_disk_entry_from_file.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_disk_posix.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_disk_set_standard_lookup.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_disk_windows.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_extract.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_extract2.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_open_fd.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_open_file.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_open_filename.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_open_memory.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_set_format.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_set_options.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_all.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_by_code.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_bzip2.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_compress.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_grzip.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_gzip.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_lrzip.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_lz4.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_lzop.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_none.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_program.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_rpm.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_uu.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_xz.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_filter_zstd.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_7zip.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_all.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_ar.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_by_code.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_cab.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_cpio.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_empty.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_iso9660.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_lha.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_mtree.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_rar.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_rar5.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_raw.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_tar.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_warc.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_xar.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_read_support_format_zip.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_string.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_string_sprintf.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_time.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_util.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_version_details.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_virtual.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_windows.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_b64encode.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_by_name.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_bzip2.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_compress.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_grzip.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_gzip.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_lrzip.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_lz4.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_lzop.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_none.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_program.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_uuencode.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_xz.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_add_filter_zstd.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_disk_posix.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_disk_set_standard_lookup.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_disk_windows.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_open_fd.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_open_file.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_open_filename.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_open_memory.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_7zip.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_ar.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_by_name.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_cpio.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_cpio_binary.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_cpio_newc.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_cpio_odc.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_filter_by_ext.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_gnutar.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_iso9660.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_mtree.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_pax.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_raw.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_shar.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_ustar.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_v7tar.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_warc.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_xar.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_format_zip.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_options.c"
        "${libarchive_SOURCE_DIR}/libarchive/archive_write_set_passphrase.c"
        "${libarchive_SOURCE_DIR}/libarchive/filter_fork_windows.c"
        "${libarchive_SOURCE_DIR}/libarchive/xxhash.c"
)

target_include_directories(diffractor_archive PUBLIC
        "${libarchive_SOURCE_DIR}/libarchive"
        "$<TARGET_PROPERTY:diffractor_bzip2,INTERFACE_INCLUDE_DIRECTORIES>"
        "${lzma_SOURCE_DIR}/src/liblzma/api"
)

# zlib comes with libfiles, and with it its headers.
target_link_libraries(diffractor_archive PUBLIC diffractor::zlib)

target_compile_definitions(diffractor_archive PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:HAVE_CONFIG_H>" "$<$<COMPILE_LANGUAGE:C,CXX>:LIBARCHIVE_STATIC>" "$<$<COMPILE_LANGUAGE:C,CXX>:LZMA_API_STATIC>")

if (WIN32)
    target_compile_definitions(diffractor_archive PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:UNICODE>" "$<$<COMPILE_LANGUAGE:C,CXX>:_UNICODE>")
endif ()

diffractor_apply_vendored_policy(diffractor_archive)

# Anything the Windows project could not describe - a header its build generates, a flag a
# different compiler needs. Hand written, and kept out of this file so that re-importing
# does not discard it.
include("${CMAKE_CURRENT_LIST_DIR}/archive.local.cmake" OPTIONAL)

add_library(diffractor::archive ALIAS diffractor_archive)
