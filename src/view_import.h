// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: File import workflow. Manages importing files from cameras,
// devices, and folders with duplicate detection and organization.

#pragma once

#include "app_util.h"
#include "view_list.h"

class import_view final : public review_list, public std::enable_shared_from_this<import_view>
{
	bool _select_other_folder = false;
	std::vector<import_source> _sources;
	std::string _title;
	import_analysis_result _analysis;
	import_options _analysis_options;
	bool _analysis_valid = false;
	// The processing generation a Run began. Refresh re-plans, and a Run still under way is executing
	// the plan already reviewed, so it is left alone rather than cancelled partway.
	size_t _run_generation = 0;
	// Where the device the plan reads was staged, and what it left on the device as imported already; nothing
	// for a plan that reads no device.
	core::folder_path _staging;
	size_t _left_on_device = 0;
	// Says to other instances that this one runs, so the staging it holds is not taken from it.
	platform::instance_lock_ptr _staging_lock;

	void invalidate_analysis();
	// The phone or camera chosen as the source, if one is.
	std::optional<platform::portable_device_t> selected_device() const;
	void clear_staging();

public:
	import_view(view_state& state, view_host_ptr host) : review_list(state, std::move(host)) { col_count = 4; }

	std::array<text_t, max_col_count> col_titles() override
	{
		return std::array<text_t, max_col_count>{
			tt.action,
			tt.source,
			{},
			tt.destination,
		};
	};

	text_t empty_message() override { return tt.view_empty_message; }

	void run();
	void analyze();
	void refresh() override;
	void reload() override;

	// Analyze needs a source to read and a destination to plan into. Without both it can only
	// repeat the failure the dimmed button already states.
	bool can_analyze() const;

	bool can_run() const
	{
		return _analysis_valid && count_imports(_analysis) > 0 &&
		       !(_analysis_options.collision == collision_policy::block_run && count_import_collisions(_analysis) > 0);
	}

	void deactivate() override
	{
		abandon_processing();
		_rows.clear();
		_sources.clear();
		_status.clear();
		_analysis.clear();
		_analysis_valid = false;
		clear_staging();
	}

	// Nothing this instance stages is in use as Import opens, so its staging, and what instances that have ended left,
	// is cleared away.
	void activate(sizei extent) override;

	// What a worker reading the plan's source has got to, said on the status line while that plan is the one
	// processing.
	void show_progress(size_t generation, std::string text);

	view_controls_host_ptr controls(const ui::frame_ptr& owner);
	void populate_controls(const view_controls_host_ptr& host, const ui::frame_ptr& frame);

	std::string_view operation_name() const override { return tt.command_import; }

	void exit() override
	{
		if (!confirm_exit_while_processing(operation_name())) return;
		_state.view_mode(view_type::items);
	}

	std::string_view title() override
	{
		_title = std::format("{}: {}", core::app_name(), tt.command_import);
		return _title;
	}

	view_bars bars() const override;

	void update_rows(const import_analysis_result& analysis_result, const import_options& options);
};
