// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Batch file rename view. Provides template-based renaming with preview,
// sequential numbering, metadata-driven naming patterns, and grouped sidecar recovery.

#include "pch.h"
#include "model.h"
#include "model_index.h"
#include "app_command_status.h"
#include "app_util.h"
#include "view_rename.h"

#include "ui_controls.h"

static void notify_rename_after_move(const df::file_path source, const df::file_path destination)
{
	if (df::rename_after_move_for_test) df::rename_after_move_for_test(source, destination);
}

static platform::file_op_result rename_recovery_failed(const df::file_path expected_path,
                                                       const df::file_path surviving_path,
                                                       platform::file_op_result result)
{
	result.code = platform::file_op_result_code::FAILED;
	result.error_message = std::format("Could not restore {}; surviving path is {}. {}",
	                                   expected_path.str(), surviving_path.str(), result.format_error());
	df::log(__FUNCTION__, result.error_message);
	return result;
}

static platform::file_op_result move_rename_path(const df::file_path source, const df::file_path destination,
                                                 const bool fail_if_exists, const bool is_folder = false)
{
	if (is_folder)
	{
		const auto source_folder = source.folder().combine(source.name());
		const auto destination_folder = destination.folder().combine(destination.name());
		// A folder path packs with a trailing separator and an empty name, so the equality and
		// case-only tests have to run against the folders rather than the packed file paths.
		if (source_folder.text() == destination_folder.text()) return {platform::file_op_result_code::OK, {}, {}};
		if (source_folder.compare(destination_folder) != 0)
			return platform::move_file(source_folder, destination_folder);

		// The swap has to stage in the parent, which is destination.folder() for a folder row;
		// source.folder() is the folder being renamed.
		const auto parent = destination.folder();
		const auto temporary_folder = parent.combine(platform::temp_file({}, parent).name());
		auto result = platform::move_file(source_folder, temporary_folder);
		if (result.success())
		{
			const auto temporary_path = df::file_path(temporary_folder.parent(), temporary_folder.name());
			notify_rename_after_move(source, temporary_path);
			result = platform::move_file(temporary_folder, destination_folder);
			if (result.success())
			{
				notify_rename_after_move(temporary_path, destination);
			}
			else
			{
				const auto rollback = platform::move_file(temporary_folder, source_folder);
				if (rollback.success()) notify_rename_after_move(temporary_path, source);
				else return rename_recovery_failed(source, temporary_path, rollback);
			}
		}
		return result;
	}

	if (source.icmp(destination) != 0 || source.pack() == destination.pack())
	{
		auto result = platform::move_file(source, destination, fail_if_exists);
		if (result.success()) notify_rename_after_move(source, destination);
		return result;
	}

	const auto temporary = platform::temp_file(source.extension(), source.folder());
	auto result = platform::move_file(source, temporary, true);
	if (result.success())
	{
		notify_rename_after_move(source, temporary);
		result = platform::move_file(temporary, destination, fail_if_exists);
		if (result.success())
		{
			notify_rename_after_move(temporary, destination);
		}
		else
		{
			const auto rollback = platform::move_file(temporary, source, true);
			if (rollback.success()) notify_rename_after_move(temporary, source);
			else return rename_recovery_failed(source, temporary, rollback);
		}
	}
	return result;
}

void rename_view::run()
{
	if (!can_run()) return;

	const auto renames = _renames;
	const auto review_status = _status;
	_analysis_valid = false;
	// Releasing the displayed file's handles can pump messages, and a Close handled in there leaves
	// this view. A run started after that would rename files with nothing on screen to show it.
	const auto generation_before_detach = processing_generation();
	const auto detach = std::make_shared<detach_file_handles>(_state);
	if (processing_generation() != generation_before_detach) return;
	begin_processing(renames.size());
	const auto processing_generation = this->processing_generation();
	const auto cancel_source = processing_cancel_source();
	_status = std::string(tt.processing.sv());
	const auto results = make_run_status(shared_from_this(), processing_generation, cancel_source, review_status,
	                                     detach);

	_state.queue_async(async_queue::work, [renames, results, &index = _state.item_index]
	{
		result_scope rr(results);
		df::unique_folders scan_folders;

		// Existence alone cannot tell the reviewed file from one that replaced it since. A source
		// that changed would be moved under a name chosen for content the user never saw, and under
		// Replace a changed destination would be written over without having been reviewed. Folders
		// are held to existence only: their contents change without the folder becoming another.
		const auto reviewed_file = [](const df::file_path path, const platform::file_attributes_t& reviewed)
		{
			return reviewed.exists()
				       ? unchanged_since_analysis(path, reviewed.modified, reviewed.size)
				       : !path.exists();
		};

		for (const auto& rename : renames)
		{
			if (!rename_path_exists(rename.source, rename.is_folder) ||
				rename_path_exists(rename.destination, rename.is_folder) != rename.destination_exists)
			{
				rr.complete(tt.sync_analysis_changed);
				return;
			}

			if (!rename.is_folder &&
				(!reviewed_file(rename.source, rename.source_attributes) ||
					!reviewed_file(rename.destination, rename.destination_attributes)))
			{
				rr.complete(tt.sync_analysis_changed);
				return;
			}

			for (auto index = 0_z; index < rename.sidecars.size(); ++index)
			{
				const auto& [source, destination] = rename.sidecars[index];
				if (!source.exists() || destination.exists() != rename.sidecar_destinations_exist[index] ||
					!reviewed_file(source, rename.sidecar_source_attributes[index]) ||
					!reviewed_file(destination, rename.sidecar_destination_attributes[index]))
				{
					rr.complete(tt.sync_analysis_changed);
					return;
				}
			}
		}

		// The plan treats a destination held by a file this run renames away as free, so the run has
		// to free it rather than fail on it. The occupant moves to a temporary name in its own folder
		// and the row that owns it renames from there; anything still parked when the run ends goes
		// back where it came from. The test matches the one the plan used, or the run would park a
		// path the plan never counted as free.
		std::set<std::string, df::path_key_less> vacating;

		for (const auto& rename : renames)
		{
			const auto source_key = rename_path_key(rename.source, rename.is_folder);
			if (rename.noop || df::compare_path_key(source_key, rename_path_key(rename.destination, rename.is_folder)) == 0)
				continue;

			vacating.emplace(source_key);
			for (const auto& [source, destination] : rename.sidecars)
				if (df::compare_path_key(source.pack(), destination.pack()) != 0)
				{
					vacating.emplace(source.pack());
				}
		}

		struct parked_path
		{
			df::file_path original;
			df::file_path temporary;
			bool is_folder = false;
		};

		std::map<std::string, parked_path, df::path_key_less> parked;
		std::string first_recovery_error;

		const auto remember_recovery_error = [&](const platform::file_op_result& op)
		{
			if (op.failed() && first_recovery_error.empty()) first_recovery_error = op.format_error();
		};

		const auto remember_recovery_message = [&](const std::string& message)
		{
			if (first_recovery_error.empty()) first_recovery_error = message;
		};

		auto current_path = [&parked](const df::file_path path, const bool is_folder)
		{
			const auto found = parked.find(rename_path_key(path, is_folder));
			return found == parked.end() ? path : found->second.temporary;
		};

		auto free_destination = [&](const df::file_path destination, const bool is_folder)
		{
			const platform::file_op_result nothing_to_do{platform::file_op_result_code::OK, {}, {}};
			const auto key = rename_path_key(destination, is_folder);

			if (!vacating.contains(key) || parked.contains(key)) return nothing_to_do;
			if (!rename_path_exists(destination, is_folder)) return nothing_to_do;

			const auto temporary = platform::temp_file(is_folder ? std::string_view{} : destination.extension(),
			                                           destination.folder());
			auto result = move_rename_path(destination, temporary, true, is_folder);
			if (result.success()) parked.emplace(key, parked_path{destination, temporary, is_folder});
			return result;
		};

		for (const auto& rename : renames)
		{
			results->start_item(rename.original_name);
			if (results->is_canceled())
			{
				results->end_item(rename.original_name, item_status::cancel);
				continue;
			}
			if (rename.noop)
			{
				results->end_item(rename.original_name, item_status::ignore);
				continue;
			}

			std::vector<std::pair<df::file_path, df::file_path>> moved_sidecars;
			platform::file_op_result result{platform::file_op_result_code::OK, {}, {}};
			df::unique_folders touched_folders;
			touched_folders.emplace(rename.source.folder());
			touched_folders.emplace(rename.destination.folder());
			for (auto index = 0_z; index < rename.sidecars.size(); ++index)
			{
				const auto& [source, destination] = rename.sidecars[index];
				touched_folders.emplace(source.folder());
				touched_folders.emplace(destination.folder());
				result = free_destination(destination, false);
				if (result.failed()) break;
				result = move_rename_path(current_path(source, false), destination,
				                          rename.policy != collision_policy::replace);
				if (result.failed()) break;
				parked.erase(rename_path_key(source, false));
				// Every sidecar this row moved rolls back, whatever stood at the destination. A
				// rename is always a move, so under Replace the destination's former content is
				// already gone by the time the primary can fail; putting the sidecar back destroys
				// nothing further and is what leaves the group whole at its source, which is what
				// the run promises. Import cannot take this rule - a copy that replaced a
				// destination would leave that path empty rather than restored.
				moved_sidecars.emplace_back(source, destination);
			}
			if (result.success()) result = free_destination(rename.destination, rename.is_folder);
			if (result.success())
			{
				result = move_rename_path(current_path(rename.source, rename.is_folder), rename.destination,
				                          rename.policy != collision_policy::replace, rename.is_folder);
				if (result.success()) parked.erase(rename_path_key(rename.source, rename.is_folder));
			}
			if (result.failed())
			{
				for (auto sidecar = moved_sidecars.rbegin(); sidecar != moved_sidecars.rend(); ++sidecar)
				{
					const auto rollback = move_rename_path(sidecar->second, sidecar->first, true);
					if (rollback.failed()) result = rename_recovery_failed(sidecar->first, sidecar->second, rollback);
				}
			}
			if (result.failed())
			{
				remember_recovery_error(result);
				for (const auto& folder : touched_folders) scan_folders.emplace(folder);
			}
			else
			{
				scan_folders.emplace(rename.source.folder());
				scan_folders.emplace(rename.destination.folder());
			}
			results->end_item(rename.original_name, to_status(result.code));
		}

		// Recover the whole row, including companions that never needed parking. Independent
		// destinations could attach a recovered sidecar to an unrelated, occupied primary.
		for (const auto& rename : renames)
		{
			if (!parked.contains(rename_path_key(rename.source, rename.is_folder)) &&
				std::ranges::none_of(rename.sidecars, [&parked](const auto& sidecar)
				{
					return parked.contains(rename_path_key(sidecar.first, false));
				}))
			{
				continue;
			}

			struct recovery_member
			{
				df::file_path original;
				df::file_path current;
				df::file_path reviewed;
				bool is_folder;
			};
			const auto primary = rename.is_folder
				                     ? df::file_path(rename.source.folder().parent(), rename.source.folder().name())
				                     : rename.source;
			std::vector<recovery_member> group{
				{primary, current_path(rename.source, rename.is_folder), rename.destination, rename.is_folder}
			};
			for (const auto& [source, destination] : rename.sidecars)
				group.push_back({source, current_path(source, false), destination, false});

			const auto destinations_free = [&group](const std::vector<df::file_path>& destinations)
			{
				for (size_t i = 0; i < group.size(); ++i)
				{
					const auto& member = group[i];
					if (df::compare_path_key(rename_path_key(member.current, member.is_folder),
					                         rename_path_key(destinations[i], member.is_folder)) != 0 &&
						rename_path_exists(destinations[i], member.is_folder))
						return false;
				}
				return true;
			};
			const auto recover = [&](const std::vector<df::file_path>& destinations)
			{
				platform::file_op_result recovery{platform::file_op_result_code::ALREADY_EXISTS};
				if (!destinations_free(destinations)) return recovery;
				for (const auto& member : group)
					if (!rename_path_exists(member.current, member.is_folder))
						return platform::file_op_result{platform::file_op_result_code::FAILED,
							std::format("Missing rename recovery source {}", member.current.str())};

				std::vector<std::pair<size_t, df::file_path>> moved;
				for (size_t i = 0; i < group.size(); ++i)
				{
					auto& member = group[i];
					scan_folders.emplace(member.current.folder());
					scan_folders.emplace(destinations[i].folder());
					if (rename_path_key(member.current, member.is_folder) ==
						rename_path_key(destinations[i], member.is_folder)) continue;

					recovery = move_rename_path(member.current, destinations[i], true, member.is_folder);
					if (recovery.failed())
					{
						for (auto it = moved.rbegin(); it != moved.rend(); ++it)
						{
							auto& previous = group[it->first];
							const auto rollback = move_rename_path(previous.current, it->second, true,
							                                       previous.is_folder);
							if (rollback.success()) previous.current = it->second;
							else (void)rename_recovery_failed(previous.original, previous.current, rollback);
						}
						return recovery;
					}
					moved.emplace_back(i, member.current);
					member.current = destinations[i];
				}
				return platform::file_op_result{platform::file_op_result_code::OK};
			};

			std::vector<df::file_path> destinations;
			for (const auto& member : group) destinations.emplace_back(member.original);
			auto recovery = recover(destinations);
			if (recovery.failed())
			{
				for (size_t i = 0; i < group.size(); ++i) destinations[i] = group[i].reviewed;
				recovery = recover(destinations);
			}

			auto numbered = false;
			if (recovery.failed())
			{
				const auto base = rename.is_folder ? std::string(primary.name()) :
					std::string(primary.file_name_without_extension());
				const auto extension = rename.is_folder ? ""sv : primary.extension();
				for (auto suffix = 2; suffix < 10000; ++suffix)
				{
					destinations[0] = df::file_path(primary.folder(), std::format("{} ({})", base, suffix), extension);
					for (size_t i = 1; i < group.size(); ++i)
						destinations[i] = destinations[0].extension(group[i].original.extension());
					if (!destinations_free(destinations)) continue;
					recovery = recover(destinations);
					numbered = recovery.success();
					if (recovery.code != platform::file_op_result_code::ALREADY_EXISTS) break;
				}
			}

			if (recovery.success())
			{
				for (const auto& member : group) parked.erase(rename_path_key(member.original, member.is_folder));
				if (numbered)
				{
					const auto message = std::format("Recovered {} as {}", primary.str(), destinations[0].str());
					df::log(__FUNCTION__, message);
					remember_recovery_message(message);
				}
			}
			else
			{
				for (const auto& member : group)
				{
					if (auto found = parked.find(rename_path_key(member.original, member.is_folder));
						found != parked.end())
						found->second.temporary = member.current;
					remember_recovery_error(rename_recovery_failed(member.original, member.current, recovery));
				}
			}
		}

		index.queue_scan_folders(std::move(scan_folders));
		rr.complete(first_recovery_error);
	});
}

std::string rename_view::run_blocked_reason() const
{
	// While a run is in flight, or before there is a plan to judge, the toolbar and the status
	// already state the situation; naming a second cause on top of that would be noise.
	if (progress().active || !_analysis_valid || _renames.empty()) return {};

	// A name the file system will not accept blocks the run whatever the collision policy is, and
	// the row alone cannot say which character is at fault.
	const auto unusable = std::ranges::find_if(_renames, [](const rename_item& item)
	{
		return !item.valid && !item.collides;
	});
	if (unusable != _renames.end()) return str_format(tt.error_invalid_path_fmt.sv(), unusable->new_name);

	// Block Run refuses a collision the other policies resolve, and a destination claimed twice by
	// the plan refuses under every policy. Both are counted the same way.
	const auto unresolved = count_rename_collisions(_renames, collision_policy::block_run);
	if (unresolved > 0 && !can_rename_items(_renames))
		return format_collision_summary(collision_policy::block_run, unresolved);

	return {};
}

void rename_view::refresh()
{
	// The worker reports against the reviewed rows, so re-analysing under it would repoint the run
	// at a different set of names.
	if (progress().active) return;
	_showing_results = false;

	const auto& items = _state.selected_items();
	const auto sources = snapshot_rename_sources(items);
	const auto name_template = setting.rename.name_template;
	const auto start_seq = str::to_int(setting.rename.start_seq);
	const auto policy = setting.rename.collision;
	const auto completion_status = format_plural_text(tt.rename_fmt, items);
	const auto generation = ++_analysis_generation;
	_analysis_valid = false;
	_status = std::string(tt.analyzing.sv());
	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::status |
		view_invalid::command_state);

	const std::weak_ptr<rename_view> weak_view = shared_from_this();
	_state.queue_async(async_queue::work,
	                   [&s = _state, weak_view, sources, name_template, start_seq, policy, completion_status, generation
	                   ]
	                   {
		                   auto renames = calc_item_renames(sources, name_template, start_seq, policy);
		                   s.queue_ui([weak_view, renames = std::move(renames), completion_status, generation]() mutable
		                   {
			                   const auto view = weak_view.lock();
			                   if (!view || generation != view->_analysis_generation) return;

			                   const auto error_text_color = ui::lighten(ui::style::color::warning_background, 0.55f);
			                   const auto rename_text_color = ui::lighten(
				                   ui::style::color::dialog_selected_background, 0.55f);
			                   std::vector<row_element_ptr> rows;
			                   rows.reserve(renames.size());
			                   int count = 0;
			                   for (const auto& rename : renames)
			                   {
				                   auto row = std::make_shared<row_element>(*view);
				                   row->_text[0] = rename.original_name;
				                   row->_text[2] = rename.new_name;
				                   row->_text_color[2] = rename.valid ? rename_text_color : error_text_color;
				                   if (rename.valid && !rename.noop) row->_icons[1] = icon_index::next;
				                   row->_order = count++;
				                   row->_work_index = row->_order;
				                   rows.emplace_back(std::move(row));
			                   }

			                   view->_rows = std::move(rows);
			                   view->_renames = std::move(renames);
			                   view->_analysis_valid = true;
			                   view->_status = completion_status;

			                   // A dimmed Run is only honest if the view says what would make it work,
			                   // and a resolved collision is part of what Review promises.
			                   const auto collisions = count_rename_collisions(
				                   view->_renames, setting.rename.collision);
			                   if (const auto summary = format_collision_summary(setting.rename.collision, collisions);
				                   !summary.empty())
			                   {
				                   view->_status += "   ";
				                   view->_status += summary;
			                   }
			                   else if (const auto blocked = view->run_blocked_reason(); !blocked.empty())
			                   {
				                   view->_status += "   ";
				                   view->_status += blocked;
			                   }

			                   view->_state.invalidate_view(
				                   view_invalid::view_layout | view_invalid::controller | view_invalid::status |
				                   view_invalid::command_state);
		                   });
	                   });
}

view_controls_host_ptr rename_view::controls(const ui::control_frame_ptr& owner)
{
	auto result = std::make_shared<view_controls_host>(_state);

	const auto& items = _state.selected_items();
	auto frame = owner->create_dlg(result, false);

	std::vector<std::string> folder_structure_completes
	{
		setting.rename.name_template,
		"file-###"s,
		"{created}-###"s,
		"{year}-{month}-###"s,
		"travel-{country}-###"s,
		"{artist}-{album}-{title}"s,
		"{show}.S{season}E{episode}.{title}"s,
	};

	std::sort(folder_structure_completes.begin(), folder_structure_completes.end()); // Sort
	const auto last = std::unique(folder_structure_completes.begin(), folder_structure_completes.end());
	// Move duplicates
	folder_structure_completes.erase(last, folder_structure_completes.end());
	const auto selection_thumbnails = std::make_shared<ui::selection_thumbnails_control>(frame);
	selection_thumbnails->selection_async(items.thumbs(), items.size(), _state._async);

	const auto name_template = std::make_shared<ui::edit_picker_control>(
		frame, setting.rename.name_template, folder_structure_completes,
		[this](std::string_view) { refresh(); });

	const std::vector<view_element_ptr> controls = {
		create_view_info_element(tt.rename_info),
		selection_thumbnails,
		std::make_shared<text_element>(format_plural_text(tt.rename_fmt, items)),
		std::make_shared<divider_element>(),
		std::make_shared<text_element>(tt.rename_help_template_1),
		std::make_shared<bullet_element>(icon_index::bullet, tt.rename_help_template_2),
		std::make_shared<bullet_element>(icon_index::bullet, tt.rename_help_template_3),
		std::make_shared<text_element>(tt.for_example),
		std::make_shared<bullet_element>(icon_index::bullet, tt.rename_help_template_example_2),
		std::make_shared<bullet_element>(icon_index::bullet, tt.rename_help_template_example_3),
		std::make_shared<bullet_element>(icon_index::bullet, tt.rename_help_template_example_4),
		std::make_shared<link_element>(tt.more_template_information, [] { platform::open(doc_template_url); }),
		std::make_shared<divider_element>(),
		std::make_shared<text_element>(tt.rename_template_label),
		name_template,
		std::make_shared<ui::edit_control>(frame, tt.rename_template_start_label, setting.rename.start_seq,
		                                   [this](std::string_view) { refresh(); }),
		std::make_shared<divider_element>(),
		create_collision_policy_control(frame, setting.rename.collision, [this] { refresh(); }),
	};

	for (const auto& c : controls)
	{
		c->margin.cx = 8;
		c->margin.cy = 4;
	}

	result->_controls = controls;
	result->_frame = result->_dlg = frame;
	// The template is the one thing this view exists to edit, so it opens ready for typing.
	result->initial_focus = [name_template] { name_template->focus(); };

	return result;
}
