// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Batch rename workflow. Implements rename templates with
// metadata substitution, preview, and sequential numbering.

#pragma once

#include "view_list.h"
#include "app_util.h"

using view_controls_host_ptr = std::shared_ptr<view_controls_host>;

class rename_view final :
	public list_view,
	public std::enable_shared_from_this<rename_view>
{
	std::vector<rename_item> _renames;
	std::string _title;
	uint64_t _analysis_generation = 0;
	bool _analysis_valid = false;

public:
	rename_view(view_state& state, view_host_ptr host) : list_view(state, std::move(host))
	{
		col_count = 3;
	}

	std::array<text_t, max_col_count> col_titles() override
	{
		return std::array<text_t, max_col_count>
		{
			tt.old_name,
			text_t{},
			tt.new_name,
			text_t{}
		};
	};

	text_t empty_message() override { return tt.no_items_selected_message; }

	std::string_view operation_name() const override { return tt.command_rename; }

	void exit() override
	{
		if (!confirm_exit_while_processing(operation_name())) return;
		_state.view_mode(view_type::items);
	}

	void run();
	void refresh() override;
	bool can_run() const { return _analysis_valid && can_rename_items(_renames); }
	// What would make a dimmed Run work again, empty when nothing is wrong.
	std::string run_blocked_reason() const;

	void activate(sizei extent) override
	{
		refresh();
	}

	void deactivate() override
	{
		abandon_processing();
		_rows.clear();
		_renames.clear();
		_status.clear();
		_analysis_valid = false;
		++_analysis_generation;
	}

	std::string_view title() override
	{
		_title = std::format("{}: {}", s_app_name, tt.command_rename);
		return _title;
	}

	view_controls_host_ptr controls(const ui::control_frame_ptr& owner);
};
