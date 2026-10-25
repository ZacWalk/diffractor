// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Command line argument parsing. Handles startup options like folder paths,
// file selection, no-gpu mode, and test execution flags, and the options of the headless
// tools that run instead of the window.

#pragma once

// Reads Movie projects without a window (-movie-check, -movie-import, -movie-render). Import turns
// each Movie Maker project into a native one, finding its moved sources in the collection; render
// makes a movie from each native project. docs/movie.md owns the rules.
enum class movie_batch_mode
{
	check,
	import,
	render,
};

struct movie_batch_options
{
	// A project, or a folder holding them: .wlmp to check or import, .otio to check or render.
	std::string projects;
	// Searched by name, with its subfolders, besides the collection's folders, for sources missing
	// from their stored paths.
	std::string media;
	// Where imported projects or rendered movies go. Empty puts each beside the project it came from.
	std::string output;
	movie_batch_mode mode = movie_batch_mode::check;
	bool overwrite = false;
	// Leaves out clips whose source cannot be used instead of refusing the project.
	bool skip_missing = false;
};

struct command_line_t
{
	df::file_path selection;
	df::item_selector folder_path;

	bool no_gpu = false;
	bool no_indexing = false;
	bool console_test = false;
	std::string test_filter = "*";
	std::string test_temp_folder;

	bool gen_docs = false;
	std::string docs_path;

	bool validate_po = false;

	// Read-only measurement of what perceptual-hash duplicate grouping does to a real library.
	bool dup_report = false;
	std::string dup_report_folder;
	std::string dup_report_output;

	// Runs when `projects` is set.
	movie_batch_options movie_batch;

#ifdef _DEBUG
	std::string test_action;
	std::string screenshot_scene;
	std::string screenshot_output;
#endif

	void parse(std::string_view command_line_text);
	std::string format_restart_cmd_line() const;
};

extern command_line_t command_line;

struct duplicate_report_test_photo
{
	df::file_path path;
	uint64_t created_key = 0;
	std::array<uint64_t, 4> phash{};
};

struct duplicate_report_anchor_stats
{
	size_t sets = 0;
	size_t grouped_photos = 0;
	size_t declined_capture_times = 0;
};

int run_duplicate_report(std::string_view folder_text, std::string_view output_text);
duplicate_report_anchor_stats calc_duplicate_report_anchor_stats_for_tests(
	const std::vector<duplicate_report_test_photo>& photos);

// Returns the process exit code: zero when every project ends ready to render -- checked or imported
// -- or rendered, a project or movie skipped because it is already there aside.
int run_movie_batch(const movie_batch_options& options);

namespace movie_batch_test_seams
{
	// Exercise the per-project worker without process initialization or the user's collection settings.
	bool project_is_skipped(df::file_path project, const movie_batch_options& options);
}
