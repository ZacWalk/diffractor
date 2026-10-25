/*
 * This file is part of the Diffractor photo and video organizer
 * Copyright 2026  Zac Walker
 *
 * Purpose: libarchive's configuration for glibc. The checked-in third-party config.h is a FreeBSD
 * template adapted for Windows: it typedefs mode_t, gid_t, uid_t, pid_t and id_t because Windows
 * has none, and it selects the CNG digest backend. Both are wrong here, and it cannot simply be
 * edited because it must keep describing Windows. libarchive offers PLATFORM_CONFIG_H for exactly
 * this, so this file is named there rather than shadowing config.h - a quoted include resolves
 * against the including file's own directory first, so shadowing would not have worked anyway.
 *
 * Anything genuinely uncertain is left undefined: libarchive treats a missing HAVE_ as absent and
 * falls back, so silence is the safe answer and a wrong 1 is not.
 */

#ifndef __LIBARCHIVE_CONFIG_H_INCLUDED
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1

/* Headers */
#define HAVE_CTYPE_H 1
#define HAVE_DIRENT_H 1
#define HAVE_DLFCN_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_GRP_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LANGINFO_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LINUX_FIEMAP_H 1
#define HAVE_LINUX_FS_H 1
#define HAVE_LINUX_MAGIC_H 1
#define HAVE_LINUX_TYPES_H 1
#define HAVE_LOCALE_H 1
#define HAVE_MEMORY_H 1
#define HAVE_PATHS_H 1
#define HAVE_POLL_H 1
#define HAVE_PTHREAD_H 1
#define HAVE_PWD_H 1
#define HAVE_REGEX_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_SPAWN_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_MOUNT_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_POLL_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_STATVFS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_SYSMACROS_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UTSNAME_H 1
#define HAVE_SYS_VFS_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_SYS_XATTR_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_UTIME_H 1
#define HAVE_WCHAR_H 1
#define HAVE_WCTYPE_H 1
#define TIME_WITH_SYS_TIME 1
#define MAJOR_IN_SYSMACROS 1

/* Compression libraries. Matching the Windows build: zlib and lzma in, bzip2 out. */
#define HAVE_ZLIB_H 1
#define HAVE_LIBZ 1
#define HAVE_LZMA_H 1
#define HAVE_LIBLZMA 1

/* Functions */
#define HAVE_CHOWN 1
#define HAVE_CHROOT 1
#define HAVE_CTIME_R 1
#define HAVE_DUP2 1
#define HAVE_FCHDIR 1
#define HAVE_FCHMOD 1
#define HAVE_FCHOWN 1
#define HAVE_FCNTL 1
#define HAVE_FDOPENDIR 1
#define HAVE_FORK 1
#define HAVE_FSEEKO 1
#define HAVE_FSTAT 1
#define HAVE_FSTATAT 1
#define HAVE_FSTATFS 1
#define HAVE_FSTATVFS 1
#define HAVE_FTRUNCATE 1
#define HAVE_FUTIMENS 1
#define HAVE_FUTIMES 1
#define HAVE_FUTIMESAT 1
#define HAVE_GETEUID 1
#define HAVE_GETGRGID_R 1
#define HAVE_GETGRNAM_R 1
#define HAVE_GETPID 1
#define HAVE_GETPWNAM_R 1
#define HAVE_GETPWUID_R 1
#define HAVE_GMTIME_R 1
#define HAVE_LCHOWN 1
#define HAVE_LINK 1
#define HAVE_LINKAT 1
#define HAVE_LOCALTIME_R 1
#define HAVE_LSTAT 1
#define HAVE_LUTIMES 1
#define HAVE_MBRTOWC 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_MKDIR 1
#define HAVE_MKFIFO 1
#define HAVE_MKNOD 1
#define HAVE_MKSTEMP 1
#define HAVE_NL_LANGINFO 1
#define HAVE_OPENAT 1
#define HAVE_PIPE 1
#define HAVE_POLL 1
#define HAVE_POSIX_SPAWNP 1
#define HAVE_READLINK 1
#define HAVE_READLINKAT 1
#define HAVE_SELECT 1
#define HAVE_SETENV 1
#define HAVE_SETLOCALE 1
#define HAVE_SIGACTION 1
#define HAVE_STATFS 1
#define HAVE_STATVFS 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRERROR_R 1
#define HAVE_STRFTIME 1
#define HAVE_STRNLEN 1
#define HAVE_STRRCHR 1
#define HAVE_SYMLINK 1
#define HAVE_TIMEGM 1
#define HAVE_TZSET 1
#define HAVE_UNLINKAT 1
#define HAVE_UNSETENV 1
#define HAVE_UTIME 1
#define HAVE_UTIMENSAT 1
#define HAVE_UTIMES 1
#define HAVE_VFORK 1
#define HAVE_VPRINTF 1
#define HAVE_WCRTOMB 1
#define HAVE_WCSCMP 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCTOMB 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1

/* Extended attributes */
#define ARCHIVE_XATTR_LINUX 1
#define HAVE_FGETXATTR 1
#define HAVE_FLISTXATTR 1
#define HAVE_FSETXATTR 1
#define HAVE_GETXATTR 1
#define HAVE_LGETXATTR 1
#define HAVE_LISTXATTR 1
#define HAVE_LLISTXATTR 1
#define HAVE_LSETXATTR 1
#define HAVE_SETXATTR 1

/* ACLs are deliberately absent: they would need libacl, which is a system dependency the
   fully-vendored build does not take. archive_disk_acl_linux.c then compiles to nothing. */

/* Digests and AES. Windows uses CNG; OpenSSL is the equivalent here, and libcrypto is already
   linked for minizip. */
#define HAVE_LIBCRYPTO 1
#define HAVE_OPENSSL_EVP_H 1
#define ARCHIVE_CRYPTO_MD5_OPENSSL 1
#define ARCHIVE_CRYPTO_RMD160_OPENSSL 1
#define ARCHIVE_CRYPTO_SHA1_OPENSSL 1
#define ARCHIVE_CRYPTO_SHA256_OPENSSL 1
#define ARCHIVE_CRYPTO_SHA384_OPENSSL 1
#define ARCHIVE_CRYPTO_SHA512_OPENSSL 1
#define HAVE_PKCS5_PBKDF2_HMAC_SHA1 1

/* Types and declarations */
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_STRERROR_R 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_INTMAX_T 1
#define HAVE_UINTMAX_T 1
#define HAVE_LONG_LONG_INT 1
#define HAVE_UNSIGNED_LONG_LONG 1
#define HAVE_UNSIGNED_LONG_LONG_INT 1
#define HAVE_WCHAR_T 1
#define HAVE_STRUCT_STAT_ST_BLKSIZE 1
#define HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC 1
#define HAVE_STRUCT_TM_TM_GMTOFF 1

#endif /* __LIBARCHIVE_CONFIG_H_INCLUDED */
