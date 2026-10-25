// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Folder synchronization view. Compares folders and manages
// file synchronization with conflict resolution.

#pragma once

#include "view_list.h"
#include "app_util.h"


class sync_view final :
	public list_view,
	public std::enable_shared_from_this<sync_view>
{
	bool _select_other_folder = false;
	std::string _title;
	sync_analysis_result _analysis;
	bool _analysis_valid = false;
	// The processing generation a Run began. Refresh re-plans, and a Run still under way is executing
	// the plan already reviewed, so it is left alone rather than cancelled partway.
	size_t _run_generation = 0;

	void invalidate_analysis();

public:
	sync_view(view_state& state, view_host_ptr host) : list_view(state, std::move(host))
	{
		col_count = 4;
	}

	std::array<text_t, max_col_count> col_titles() override
	{
		return std::array<text_t, max_col_count>
		{
			tt.action,
			tt.local,
			{},
			tt.remote,
		};
	};

	text_t empty_message() override { return tt.view_empty_message; }

	void run();
	void analyze();
	void refresh() override;
	void reload() override;

	// Analyze needs both sides of the comparison. Without them it can only repeat the failure the
	// dimmed button already states.
	bool can_analyze() const;

	bool can_run() const { return _analysis_valid && count_sync_actions(_analysis) > 0; }

	void deactivate() override
	{
		abandon_processing();
		_rows.clear();
		_status.clear();
		_analysis.clear();
		_analysis_valid = false;
	}

	view_controls_host_ptr controls(const ui::control_frame_ptr& owner);

	std::string_view operation_name() const override { return tt.command_sync; }

	void exit() override
	{
		if (!confirm_exit_while_processing(operation_name())) return;
		_state.view_mode(view_type::items);
	}

	std::string_view title() override
	{
		_title = std::format("{}: {}", s_app_name, tt.command_sync);
		return _title;
	}

	void update_rows(const sync_analysis_result& analysis_result);
};
