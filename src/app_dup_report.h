// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The duplicate report: a read-only measurement, over a real library, of what perceptual-hash duplicate
// grouping does, which the duplicate_report tool runs (app_mcp_tools.cpp).

#pragma once

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

// Reports to print as it goes, and to the output file too where one is named; zero once the report is complete.
int run_duplicate_report(std::string_view folder_text, std::string_view output_text,
                         const std::function<void(std::string_view)>& print);
duplicate_report_anchor_stats calc_duplicate_report_anchor_stats_for_tests(
	const std::vector<duplicate_report_test_photo>& photos);
