# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: What the liblzma .vcxproj could not describe. The checked-in config.h is the one its
# configure produced for Windows, and it names the Win95 and Vista threading backends. mythread.h
# tests for POSIX first, so asking for that is enough - the Windows names stay defined but are
# never reached, and no second config.h has to be maintained.

if (NOT WIN32)
    target_compile_definitions(diffractor_lzma PRIVATE MYTHREAD_POSIX)
endif ()
