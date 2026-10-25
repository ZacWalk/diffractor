# This file is part of the Diffractor photo and video organizer
# Copyright 2026  Zac Walker
#
# Purpose: Acknowledge the legacy MSVC preprocessor mode inherited from Diffractor's shipped
# configuration. OpenMPT diagnoses it in every translation unit; this suppresses only that message.

if (MSVC)
    target_compile_definitions(diffractor_openmpt PRIVATE MPT_CHECK_CXX_IGNORE_PREPROCESSOR)
endif ()