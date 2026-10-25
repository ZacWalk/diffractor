// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tagging workflow view. Adds and removes tags across the visibly selected set
// using the same selector, preview, run and results steps as the other task views.

#pragma once

#include "view_list.h"

using view_controls_host_ptr = std::shared_ptr<view_controls_host>;

// One entry of the tag action field: the tag and whether it is being added (true) or removed.
using tag_action = std::pair<std::string, bool>;

// The action field is a list of terms, each optionally prefixed with '-' to remove. Parsing
// keeps the last modifier seen for a tag and matches tags case-insensitively; serializing is
// the exact inverse, quoting any tag that contains white space (issue #228).
std::vector<tag_action> parse_tag_actions(std::string_view text);
std::string serialize_tag_actions(const std::vector<tag_action>& actions);

class tags_view final :
	public list_view,
	public std::enable_shared_from_this<tags_view>
{
	// The action field: what to add or remove, not a view of current state (issue #228).
	std::string _tags;

	std::vector<std::string> _adds;
	std::vector<std::string> _removes;

	// Backing storage for the string_views handed to the recommended-word toolbars.
	std::vector<std::string_view> _favorite_words;
	std::vector<std::string_view> _common_words;
	std::vector<std::string_view> _existing_words;
	df::string_counts _common_counts;
	df::string_counts _existing_counts;

	std::shared_ptr<ui::multi_line_edit_control> _edit;
	ui::recommended_words_control::refresh_group _word_refresh;

	std::string _title;

public:
	tags_view(view_state& state, view_host_ptr host) : list_view(state, std::move(host))
	{
		col_count = 3;
	}

	std::array<text_t, max_col_count> col_titles() override
	{
		return std::array<text_t, max_col_count>
		{
			tt.tags_column_item,
			text_t{},
			tt.tags_column_result,
			text_t{}
		};
	}

	text_t empty_message() override { return tt.tags_view_empty_message; }

	std::string_view operation_name() const override { return tt.tag_add_remove; }

	void exit() override
	{
		if (!confirm_exit_while_processing(operation_name())) return;
		_state.view_mode(view_type::items);
	}

	void run();
	void refresh() override;
	bool can_run() const;

	void activate(const sizei extent) override
	{
		list_view::activate(extent);
		refresh();
	}

	void deactivate() override
	{
		abandon_processing();
		_rows.clear();
		_adds.clear();
		_removes.clear();
		_status.clear();
	}

	std::string_view title() override
	{
		_title = std::format("{}: {}", s_app_name, tt.tag_add_remove);
		return _title;
	}

	view_controls_host_ptr controls(const ui::control_frame_ptr& owner);

private:
	void parse_tags();
	void toggle_action(std::string_view tag, bool positive);
	bool contains_action(std::string_view tag, bool positive) const;
	void refresh_words() const;
};
