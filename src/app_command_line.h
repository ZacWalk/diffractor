// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Command line argument parsing. Handles startup options like folder paths,
// file selection, no-gpu mode, and test execution flags, and the options of the headless
// tools that run instead of the window.

#pragma once

// Reads Movie projects without a window (-movie-check, -movie-render). Render makes a movie from each
// .dfv project; check reports what render would find, and writes nothing. docs/movie.md owns the rules.
enum class movie_batch_mode
{
	check,
	render,
};

struct movie_batch_options
{
	// A .dfv project, or a folder holding them.
	std::string projects;
	// Where rendered movies go. Empty puts each beside the project it came from.
	std::string output;
	movie_batch_mode mode = movie_batch_mode::check;
	bool overwrite = false;
	// Leaves out clips whose source cannot be used instead of refusing the project.
	bool skip_missing = false;
};

struct command_line_t
{
	core::file_path selection;
	core::item_selector folder_path;

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
	// Records the window to an MP4 (-record:<file>) from when it first shows until it closes, or for
	// record_seconds (-record-seconds:<n>) and then closes it, at a client size of record_size
	// (-record-size:<width>x<height>) when one is given.
	std::string record_output;
	double record_seconds = 0;
	sizei record_size;
	// Keeps settings, the index and the map's tiles in this folder instead of the user's (-profile:<folder>).
	std::string profile_folder;
	// The collection is this folder and no other, and the window opens on it (-collection:<folder>). With no
	// -profile it implies -profile:<folder>-profile, so the user's own collection settings are never replaced.
	std::string collection_folder;
	// Runs a tutorial script (-tutorial:<file.dft>): app_tutorial.h has its grammar.
	std::string tutorial_script;
#endif

	void parse(std::string_view command_line_text);
	std::string format_restart_cmd_line() const;
};

extern command_line_t command_line;

struct duplicate_report_test_photo
{
	core::file_path path;
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

// Returns the process exit code: zero when every project ends ready to render -- checked -- or
// rendered, a movie skipped because it is already there aside.
int run_movie_batch(const movie_batch_options& options);

namespace movie_batch_test_seams
{
	// Exercises the per-project worker without process initialization.
	bool project_is_skipped(core::file_path project, const movie_batch_options& options);
	// Whether -movie-check finds the project ready to render, as -movie-skip-missing would have it if asked.
	bool project_is_ready(core::file_path project, bool skip_missing = false);
}
