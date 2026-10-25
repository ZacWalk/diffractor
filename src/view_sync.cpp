// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Folder synchronization view. Compares local folders against a remote folder, reviews
// every copy and delete, and runs the reviewed plan.
//
// Analyze -> Review -> Run, per docs/design.md. Analyze builds a plan on a worker from a snapshot of
// both trees. Review is what the user reads and approves. Run executes that plan and nothing else:
// it never widens the plan, and it never acts on anything the user did not see. Changing any folder
// or option invalidates the plan, so Run cannot act on one that no longer matches the controls.
//
// Run does not re-derive the plan. Each row is revalidated against its own files immediately before
// that row acts - see sync_copy in app_util.cpp and docs/file-io.md section 9.1. This keeps Run's
// cost proportional to the work rather than to the size of the two trees, which is the difference
// between a pause and a second full scan when the remote is a NAS. A row whose files moved on writes
// nothing and is reported as failed with the reason stated; the rows that still match still run, so
// one changed file no longer discards a whole run.
//
// Deletes here are permanent. platform::delete_file bypasses the Recycle Bin, and a remote UNC path
// has no Recycle Bin to bypass. That is why deletion is confirmed separately, with its permanence
// stated, even though every deletion is already a row the user read.

#include "pch.h"
#include "model.h"
#include "model_index.h"
#include "ui_dialog.h"
#include "view_sync.h"
#include "app_util.h"

uint32_t count_sync_actions(const sync_analysis_result& analysis)
{
	uint32_t result = 0;

	for (const auto& i : analysis)
	{
		for (const auto& f : i.second)
		{
			if (f.second.action != sync_action::none)
			{
				++result;
			}
		}
	}

	return result;
}

uint32_t count_sync_actions(const sync_analysis_result& analysis, const sync_action action)
{
	uint32_t result = 0;
	for (const auto& folder : analysis)
	{
		result += static_cast<uint32_t>(std::ranges::count_if(folder.second,
		                                                      [action](const auto& item)
		                                                      {
			                                                      return item.second.action == action;
		                                                      }));
	}
	return result;
}

static auto calc_sync_source(const df::index_roots& roots, const bool select_other_folder)
{
	df::index_roots result;

	if (select_other_folder)
	{
		result.folders.emplace(df::folder_path(setting.sync.local_path));
	}
	else
	{
		result = roots;
	}

	return result;
};

bool sync_view::can_analyze() const
{
	if (setting.sync.remote_path.empty()) return false;

	// Mirrors calc_sync_source: the collection branch answers from the indexed roots, the other
	// branch from the folder the user picked.
	return _select_other_folder
		       ? !setting.sync.local_path.empty()
		       : _state.item_index.has_index_roots();
}

void sync_view::invalidate_analysis()
{
	_analysis.clear();
	_analysis_valid = false;
	_rows.clear();
	_status.clear();
	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::status |
		view_invalid::command_state);
}

view_controls_host_ptr sync_view::controls(const ui::control_frame_ptr& owner)
{
	auto result = std::make_shared<view_controls_host>(_state);
	auto frame = owner->create_dlg(result, false);

	std::vector<view_element_ptr> controls;

	// Sync compares two folder trees, so the item selection is not its target and showing it as one
	// misstates what will be copied. The example belongs to the sentence that explains the view.
	controls.emplace_back(create_view_info_element(std::format("{} {} {}",
	                                                           tt.sync_info_1.sv(), tt.for_example.sv(),
	                                                           tt.sync_info_2.sv())));
	controls.emplace_back(std::make_shared<text_element>(tt.sync_local));

	// Initialise _select_other_folder from the persisted setting so analyze()
	// and run() see the user's last choice when controls() is rebuilt.
	_select_other_folder = !setting.sync.sync_collection;

	// Bind directly to the persisted setting and member field so toggle state
	// survives the lifetime of this controls() call (the previous code bound
	// the check to a stack-local bool, so changes were silently discarded).
	auto collection_check = std::make_shared<ui::check_control>(
		frame, tt.sync_collection, setting.sync.sync_collection, true, false,
		[this](const bool checked)
		{
			invalidate_analysis();
			if (checked)
			{
				setting.sync.sync_collection = true;
				_select_other_folder = false;
			}
		}, ui::radio_group_scope);
	controls.emplace_back(collection_check);

	auto other_folder_check = std::make_shared<ui::check_control>(
		frame, tt.sync_other_folder, _select_other_folder, true, false,
		[this](const bool checked)
		{
			invalidate_analysis();
			if (checked)
			{
				setting.sync.sync_collection = false;
				_select_other_folder = true;
			}
		}, ui::radio_group_scope);
	other_folder_check->child(std::make_shared<ui::folder_picker_control>(frame, setting.sync.local_path, false,
	                                                                      [this](const std::string_view)
	                                                                      {
		                                                                      invalidate_analysis();
	                                                                      }));
	controls.emplace_back(other_folder_check);

	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<text_element>(tt.sync_remote));
	controls.emplace_back(std::make_shared<ui::folder_picker_control>(frame, setting.sync.remote_path, false,
	                                                                  [this](const std::string_view)
	                                                                  {
		                                                                  invalidate_analysis();
	                                                                  }));

	// These choose what sync does in each direction; without a break they read as options of the
	// remote folder picker above them.
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(
		std::make_shared<ui::check_control>(frame, tt.sync_local_remote, setting.sync.sync_local_remote,
		                                    false, false, [this](const bool) { invalidate_analysis(); }));
	controls.emplace_back(
		std::make_shared<ui::check_control>(frame, tt.sync_remote_local, setting.sync.sync_remote_local,
		                                    false, false, [this](const bool) { invalidate_analysis(); }));
	controls.emplace_back(
		std::make_shared<ui::check_control>(frame, tt.sync_delete_local, setting.sync.sync_delete_local,
		                                    false, false, [this](const bool) { invalidate_analysis(); }));
	controls.emplace_back(
		std::make_shared<ui::check_control>(frame, tt.sync_delete_remote, setting.sync.sync_delete_remote,
		                                    false, false, [this](const bool) { invalidate_analysis(); }));

	for (const auto& c : controls)
	{
		c->margin.cx = 8;
		c->margin.cy = 4;
	}

	result->_controls = controls;
	result->_frame = result->_dlg = frame;

	return result;
}

void sync_view::update_rows(const sync_analysis_result& analysis_result)
{
	const auto blue_text_color = ui::lighten(ui::style::color::dialog_selected_background, 0.55f);
	const auto gray_text_color = ui::darken(ui::style::color::view_text, 0.22f);
	const auto orange_text_color = ui::lighten(ui::style::color::important_background, 0.55f);

	std::vector<row_element_ptr> rows;

	int ignore = 0;
	int copy_local = 0;
	int copy_remote = 0;
	int replace_count = 0;
	int delete_local_count = 0;
	int delete_remote_count = 0;
	int work_index = 0;

	for (const auto& a : analysis_result)
	{
		for (const auto& i : a.second)
		{
			auto row = std::make_shared<row_element>(*this);

			switch (i.second.action)
			{
			case sync_action::none:
				row->_text_color[0] = gray_text_color;
				row->_text_color[1] = gray_text_color;
				row->_text_color[3] = gray_text_color;
				// A remote-only file that no local root claims has no local path to state.
				if (!i.second.local_path.is_empty()) row->_text[1] = i.second.local_path.pack();
				row->_text[3] = i.second.remote_path.pack();
				row->_text[0] = tt.ignore;
				row->_order = 100;
				ignore += 1;
				break;
			case sync_action::copy_local:
				row->_text_color[0] = blue_text_color;
				row->_text_color[1] = blue_text_color;
				row->_text[0] = tt.sync_copy_local_action;
				if (!i.second.local_fi.name.is_empty())
				{
					row->_text[0] = std::format("{} ({})", tt.sync_copy_local_action.sv(), tt.collision_replace.sv());
					++replace_count;
				}
				row->_text[1] = i.second.local_path.pack();
				row->_icons[2] = icon_index::back;
				row->_text[3] = i.second.remote_path.pack();
				row->_order = 1;
				copy_local += 1;
				break;
			case sync_action::copy_remote:
				row->_text_color[0] = blue_text_color;
				row->_text_color[3] = blue_text_color;
				row->_text[0] = tt.sync_copy_remote_action;
				if (!i.second.remote_fi.name.is_empty())
				{
					row->_text[0] = std::format("{} ({})", tt.sync_copy_remote_action.sv(), tt.collision_replace.sv());
					++replace_count;
				}
				row->_text[1] = i.second.local_path.pack();
				row->_icons[2] = icon_index::next;
				row->_text[3] = i.second.remote_path.pack();
				row->_order = 2;
				copy_remote += 1;
				break;
			case sync_action::delete_local:
				row->_text_color[0] = orange_text_color;
				row->_text_color[1] = orange_text_color;
				row->_text[0] = tt.sync_delete_local_action;
				row->_text[1] = i.second.local_path.pack();
				row->_order = 3;
				delete_local_count += 1;
				break;
			case sync_action::delete_remote:
				row->_text_color[0] = orange_text_color;
				row->_text_color[3] = orange_text_color;
				row->_text[0] = tt.sync_delete_remote_action;
				row->_text[3] = i.second.remote_path.pack();
				row->_order = 4;
				delete_remote_count += 1;
				break;
			}

			// Rows are built in the order the run consumes the same analysis, so the count of acting
			// rows so far is the work index the run will report. Ignored rows are never reported.
			if (row->_order != 100) row->_work_index = work_index++;
			rows.emplace_back(row);
		}
	}

	_rows = std::move(rows);
	_analysis = analysis_result;
	_analysis_valid = true;
	_status = std::format("{} {}   {} {}   {} {}   {} {}   {} {}",
	                      copy_local, tt.sync_copy_local_action,
	                      copy_remote, tt.sync_copy_remote_action,
	                      delete_local_count, tt.sync_delete_local_action,
	                      delete_remote_count, tt.sync_delete_remote_action,
	                      ignore, tt.ignore);
	if (replace_count > 0)
	{
		_status += "   ";
		_status += format_collision_summary(collision_policy::replace, replace_count);
	}

	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::status |
		view_invalid::command_state);
}


void sync_view::run()
{
	if (!can_run()) return;

	const auto title = tt.command_sync;
	constexpr auto icon = icon_index::sync;

	record_feature_use(features::sync);
	const auto delete_local_count = count_sync_actions(_analysis, sync_action::delete_local);
	const auto delete_remote_count = count_sync_actions(_analysis, sync_action::delete_remote);

	// Deletion is the one action here with no recovery, so it is reconfirmed against counts taken from
	// the plan about to run, immediately before it runs. The review already listed every deletion; this
	// exists to state the permanence, which a row cannot.
	if (delete_local_count > 0 || delete_remote_count > 0)
	{
		const auto confirm = make_dlg(_host->owner());
		std::vector<view_element_ptr> controls;
		controls.emplace_back(set_margin(std::make_shared<ui::title_control2>(
			confirm->_frame, icon, title, tt.sync_delete_warning)));
		controls.emplace_back(set_margin(std::make_shared<text_element>(
			str_format(tt.sync_delete_count_fmt.sv(), delete_local_count, delete_remote_count))));
		controls.emplace_back(std::make_shared<divider_element>());
		controls.emplace_back(std::make_shared<ui::ok_cancel_control>(confirm->_frame, tt.button_sync));
		if (confirm->show_modal(controls) != ui::close_result::ok) return;
	}

	// Track the remote sync target as a recent folder (previously this added the
	// import destination path which has no relevance to sync).
	if (!setting.sync.remote_path.empty())
	{
		_state.recent_folders.add(setting.sync.remote_path);
	}

	// The plan is moved out and marked invalid before any work starts, so the reviewed rows are the
	// only thing the worker can act on and a second Run cannot reuse a plan already being run. After a
	// run the user must Analyze again, which is also the recovery path for any row that was refused.
	const auto analysis_result = std::move(_analysis);
	const auto completion_status = _status;
	_analysis_valid = false;
	const auto total = count_sync_actions(analysis_result);
	begin_processing(total);
	const auto processing_generation = this->processing_generation();
	_run_generation = processing_generation;
	const auto cancel_source = processing_cancel_source();
	auto token = df::cancel_token(*cancel_source);
	_status = std::string(tt.processing.sv());
	const auto detach = std::make_shared<detach_file_handles>(_state);
	const auto view = shared_from_this();
	const auto results = make_run_status(view, processing_generation, cancel_source, completion_status);

	_state.queue_async(async_queue::work, [results, analysis_result, token, detach,
		                   &index = _state.item_index]
	                   {
		                   result_scope rr(results);

		                   // Sync writes into and deletes from the collection, so the index has to be told what changed
		                   // or the files it just copied stay invisible and the ones it deleted stay listed.
		                   const auto run_result = sync_copy(results, analysis_result, token);
		                   index.queue_scan_folders(run_result.folders_changed);

		                   // A refused row is a file that moved on, not a disk error, and the two are
		                   // indistinguishable from the row alone. Without this the user would read a
		                   // partial run as a partly broken one.
		                   rr.complete(run_result.refused > 0 ? tt.sync_analysis_changed.sv() : std::string_view{});
	                   });
}

void sync_view::analyze()
{
	const auto sync_source = calc_sync_source(_state.item_index.index_roots(), _select_other_folder);
	const df::folder_path remote_path(setting.sync.remote_path);
	const auto local_remote = setting.sync.sync_local_remote;
	const auto remote_local = setting.sync.sync_remote_local;
	const auto delete_local = setting.sync.sync_delete_local;
	const auto delete_remote = setting.sync.sync_delete_remote;
	_analysis_valid = false;
	begin_processing(0);
	const auto processing_generation = this->processing_generation();
	const auto cancel_source = processing_cancel_source();
	auto token = df::cancel_token(*cancel_source);
	_status = std::string(tt.analyzing.sv());

	// The token points into cancel_source without owning it, and the next Analyze replaces the view's
	// copy while this one may still be scanning, so the worker holds the source for as long as the token.
	_state.queue_async(async_queue::work, [&s = _state, sync_source, remote_path,
		                   local_remote, remote_local, delete_local, delete_remote, t = shared_from_this(), token,
		                   cancel_source, processing_generation]
	                   {
		                   sync_analysis_result analysis_result;
		                   try
		                   {
			                   analysis_result = sync_analysis(sync_source, remote_path,
			                                                   local_remote, remote_local, delete_local, delete_remote,
			                                                   token);
		                   }
		                   catch (const std::exception& e)
		                   {
			                   s.queue_ui([t, processing_generation, error = str::utf8_cast(e.what())]
			                   {
				                   if (!t->is_processing_generation(processing_generation)) return;
				                   t->end_processing();
				                   t->_analysis.clear();
				                   t->_analysis_valid = false;
				                   t->_status = error;
				                   t->_state.invalidate_view(view_invalid::status | view_invalid::command_state);
			                   });
			                   return;
		                   }

		                   if (!token.is_cancelled() && analysis_result.valid)
		                   {
			                   s.queue_ui([analysis_result, t, processing_generation]
			                   {
				                   if (!t->is_processing_generation(processing_generation)) return;
				                   t->end_processing();
				                   t->update_rows(analysis_result);
			                   });
		                   }
		                   else if (!token.is_cancelled())
		                   {
			                   // Report why the inputs could not be analyzed. These are ordinary configuration
			                   // mistakes, not an internal fault, so the message must name the cause.
			                   s.queue_ui([t, processing_generation, error = sync_invalid_message(analysis_result)]
			                   {
				                   if (!t->is_processing_generation(processing_generation)) return;
				                   t->end_processing();
				                   t->_analysis.clear();
				                   t->_analysis_valid = false;
				                   t->_status = error;
				                   t->_state.invalidate_view(view_invalid::status | view_invalid::command_state);
			                   });
		                   }
		                   else
		                   {
			                   s.queue_ui([t, processing_generation]
			                   {
				                   if (!t->is_processing_generation(processing_generation)) return;
				                   t->end_processing();
				                   t->_analysis.clear();
				                   t->_analysis_valid = false;
				                   t->_status = std::string(tt.error_analysis_cancelled.sv());
				                   t->_state.invalidate_view(view_invalid::status | view_invalid::command_state);
			                   });
		                   }
	                   });
}

void sync_view::refresh()
{
	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::command_state);
}

void sync_view::reload()
{
	// Refresh re-plans, which restarts an Analyze in progress. A Run in progress is left alone:
	// analyzing again would cancel it partway through without reporting what it had done.
	if (progress().active && is_processing_generation(_run_generation)) return;
	analyze();
}
