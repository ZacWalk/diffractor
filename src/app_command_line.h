// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Command line argument parsing: the folder or file to open, the startup options a user or Windows' restart
// passes, the test suite's switches, an agent session (-mcp) and its options, a single tool call (-tool:), and the
// profile and collection a run keeps apart from the user's.

#pragma once

struct command_line_t
{
	core::file_path selection;
	core::item_selector folder_path;

	bool no_gpu = false;
	bool no_indexing = false;
	bool new_window = false;
	bool console_test = false;
	std::string test_filter = "*";
	std::string test_temp_folder;
	bool validate_po = false;

	// -mcp: serve tools to an agent on stdin and stdout (app_mcp.h); -mcp-ui: where its window is, and -mcp-allow:
	// what beyond reading it may do. docs/agents.md owns the session.
	bool agent = false;
	std::string agent_ui;
	std::string agent_allow;
	// -tool:<name> key=value...: one tool, called once without a session, its result printed as JSON.
	std::string tool;
	std::vector<std::string> tool_arguments;

	// Keeps settings, the index and the map's tiles in this folder instead of the user's (-profile:<folder>).
	std::string profile_folder;
	// The collection is this folder and no other, and the window opens on it (-collection:<folder>). With no
	// -profile it implies -profile:<folder>-profile, so the user's own collection settings are never replaced.
	std::string collection_folder;

	void parse(std::string_view command_line_text);
	std::string format_restart_cmd_line() const;
	bool can_forward_file() const;
};

extern command_line_t command_line;
