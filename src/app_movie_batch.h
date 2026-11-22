// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Checking and rendering Movie projects without the window: what a render would find in each .dfv, reported
// as lines a person reads and as a report a tool returns, and the render itself. docs/movie.md owns the rules; the
// movie_check and movie_render tools (app_mcp_tools.cpp) are how it is asked for.

#pragma once

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

// What one project came to.
struct movie_project_report
{
	core::file_path project;
	// ready, rendered, skipped or failed.
	std::string outcome = "failed";
	// Why it was skipped or failed, or why it is not ready.
	std::vector<std::string> reasons;

	struct line_problem
	{
		int line = 0;
		std::string reason;
	};

	std::vector<line_problem> unreadable_lines;
	size_t videos = 0;
	size_t photos = 0;
	size_t audio = 0;
	size_t text_screens = 0;
	size_t captions = 0;
	size_t stabilize = 0;
	// A YouTube Short: upright, and three minutes at most.
	bool youtube_short = false;
	double seconds = 0.0;
	sizei size;
	int frame_rate = 0;
	std::vector<core::file_path> missing;
	std::vector<core::file_path> unreadable;
	std::vector<core::file_path> sounds_missing;
	std::vector<core::file_path> sounds_unreadable;
	// Clips that play nothing, and sound files their clips cut short, as the report words them.
	std::vector<std::string> warnings;
	// The movie written, or the one a render would write.
	core::file_path movie;
};

struct movie_batch_report
{
	std::vector<movie_project_report> projects;
	// Every project ended ready to render, or rendered; a movie skipped because it is already there aside.
	bool succeeded = false;
	// Why no project was attempted.
	std::string failure;
};

// Reports each line to print as it goes, and how far a render has got to progress, from 0 to 1 across every project.
// A render stops at cancel, leaving nothing behind.
movie_batch_report run_movie_batch(const movie_batch_options& options,
                                   const std::function<void(std::string_view)>& print,
                                   const core::cancel_token& cancel = {},
                                   const std::function<void(double)>& progress = {});

// The report as a JSON object, for a tool's structured result.
std::string format_movie_batch_report(const movie_batch_report& report);

namespace movie_batch_test_seams
{
	// Exercises the per-project worker without process initialization.
	bool project_is_skipped(core::file_path project, const movie_batch_options& options);
	// Whether a check finds the project ready to render, as skip_missing would have it if asked.
	bool project_is_ready(core::file_path project, bool skip_missing = false);
}
