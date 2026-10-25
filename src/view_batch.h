// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Shared preview and processing view for batch conversion, metadata, and date tools.

#pragma once

#include "view_list.h"
#include "app_util.h"

enum class batch_tool_mode
{
	convert,
	metadata,
	adjust_date,
};

class batch_tool_view final : public list_view, public std::enable_shared_from_this<batch_tool_view>
{
	batch_tool_mode _mode = batch_tool_mode::convert;
	std::string _title;
	std::vector<convert_item_plan> _convert_plan;
	// The plan probes the filesystem on a worker, so the rows can be describing an older one.
	bool _plan_valid = false;
	uint64_t _plan_generation = 0;
	// What a result summary calls each reviewed row, in row order.
	std::vector<std::string> _row_names;
	df::date_t _original_start;
	df::date_t _new_start;
	bool _new_start_seeded = false;
	std::string _metadata_title;
	std::string _metadata_comment;
	std::string _metadata_synopsis;
	int _metadata_rating = 0;
	int _metadata_year = 0;
	df::date_t _metadata_date_original;
	df::date_t _metadata_date_created;
	df::xy8 _metadata_episode = {0, 0};
	int _metadata_season = 0;
	df::xy8 _metadata_track = {0, 0};
	df::xy8 _metadata_disk = {0, 0};
	metadata_edits metadata_changes() const;
	void update_date_start();
	void refresh_convert();
	// Rebuilds the convert rows from the plan already in hand. Touches no filesystem.
	void describe_convert_plan();
	void refresh_metadata();
	void refresh_dates();
	// Names on the status line whatever is stopping Run, so the view and the toolbar agree.
	void append_blocked_reason();
	void run_convert();
	void run_metadata();
	void run_dates();
	// Replaces the status column of every reviewed row with what the finished run actually did.
	void show_run_results(const std::vector<item_status>& statuses, std::string_view error);
	// Called from the worker: hands a finished run back to the UI thread as one status per row.
	static void queue_run_results(const view_state& s, const std::shared_ptr<batch_tool_view>& view, size_t generation,
	                              std::vector<item_status> statuses, std::string fatal, std::string first_error,
	                              std::shared_ptr<detach_file_handles> detach, std::string title);

public:
	batch_tool_view(view_state& state, view_host_ptr host) : list_view(state, std::move(host)) { col_count = 3; }

	void mode(const batch_tool_mode value) { _mode = value; }
	batch_tool_mode mode() const { return _mode; }
	void run();
	text_t run_text() const;
	void refresh() override;
	// Encoding settings cannot move a destination, so they re-describe the reviewed rows instead of
	// paying for a fresh existence probe of every one of them.
	void refresh_encode_settings();
	void activate(sizei extent) override;
	void deactivate() override;
	bool can_run() const;
	// Why Run is refused, or empty when it is available. The status line and the dimmed button both
	// answer from here, so they cannot disagree about what would make the tool work.
	std::string run_blocked_reason() const;
	// Refresh means one thing in every mode: re-plan from what is on disk now. That is worth doing
	// during review, and it is the only way out of results mode, so it is offered whenever idle.
	bool can_refresh() const { return !progress().active && showing_results(); }
	view_controls_host_ptr controls(const ui::control_frame_ptr& owner);
	std::string_view title() override;
	text_t empty_message() override { return tt.no_items_selected_message; }
	std::array<text_t, max_col_count> col_titles() override;
	std::string_view operation_name() const override;

	void exit() override
	{
		if (!confirm_exit_while_processing(operation_name())) return;
		_state.view_mode(view_type::items);
	}
};
