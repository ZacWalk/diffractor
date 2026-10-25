// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Embedded test registration and command-line test runner declarations.

#pragma once

#include "test.h"

// One registration function per subject file. The taxonomy -- which subject owns which test -- is
// documented in docs/testing.md; a test whose subject moves moves file with it.
void register_app_tests(view_state& state, test_registry& tests);
void register_av_tests(view_state& state, test_registry& tests);
void register_files_tests(view_state& state, test_registry& tests);
void register_index_tests(view_state& state, test_registry& tests);
void register_location_tests(view_state& state, test_registry& tests);
void register_media_edit_tests(view_state& state, test_registry& tests);
void register_metadata_tests(view_state& state, test_registry& tests);
void register_movie_tests(view_state& state, test_registry& tests);
void register_platform_tests(view_state& state, test_registry& tests);
void register_render_tests(view_state& state, test_registry& tests);
void register_search_tests(view_state& state, test_registry& tests);
void register_text_tests(view_state& state, test_registry& tests);
void register_util_tests(view_state& state, test_registry& tests);
void register_view_tests(view_state& state, test_registry& tests);

inline void register_tests(view_state& state, test_registry& registry)
{
	register_util_tests(state, registry);
	register_text_tests(state, registry);
	register_render_tests(state, registry);
	register_files_tests(state, registry);
	register_metadata_tests(state, registry);
	register_media_edit_tests(state, registry);
	register_av_tests(state, registry);
	register_index_tests(state, registry);
	register_search_tests(state, registry);
	register_location_tests(state, registry);
	register_movie_tests(state, registry);
	register_view_tests(state, registry);
	register_app_tests(state, registry);
	register_platform_tests(state, registry);
}

int run_console_tests(std::string_view test_filter = "*");
