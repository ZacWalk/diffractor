# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: What the sqlite .vcxproj could not describe. It states SQLITE_WIN32_MALLOC
# unconditionally because it only ever built for Windows.

# That define selects the Win32 heap allocator, and off Windows the backend behind it compiles to
# nothing at all -- leaving sqlite3MemSetDefault undefined at link time. Dropped rather than the
# whole definition list restated here, so a re-import keeps owning the rest.
if (NOT WIN32)
    get_target_property(_sqlite_defs diffractor_sqlite COMPILE_DEFINITIONS)
    list(FILTER _sqlite_defs EXCLUDE REGEX "SQLITE_WIN32_")
    set_property(TARGET diffractor_sqlite PROPERTY COMPILE_DEFINITIONS "${_sqlite_defs}")
endif ()
