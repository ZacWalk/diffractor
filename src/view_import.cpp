// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: File import workflow. Scans sources, previews every copy or move into a folder structure
// built from metadata, and runs the reviewed plan.
//
// Analyze -> Review -> Run, per docs/design.md. Analyze reads the sources on a worker and resolves
// every destination, including the collision policy and the auto-rename suffix, so Review states the
// exact path each file will land at. Run executes that plan and nothing else.
//
// Run uses _analysis_options - the options as they stood when the plan was built - rather than the
// current settings, so a control changed after Analyze cannot alter what Run does. Every control also
// invalidates the plan, so that case should not arise; reading the reviewed copy makes it impossible
// rather than merely unlikely.
//
// Run does not re-scan the sources or re-derive the plan. Each row is revalidated against its own
// files immediately before that row acts - see import_copy in app_util.cpp and docs/file-io.md#contract -
// which matters most here because the source is usually the slowest device involved, a
// card reader or a phone. The collision policy decides how much proving Run has to do: every policy
// except Replace leaves the destination free, and a fail-if-exists write proves that atomically with
// no window for a file to appear, so only Replace has to check that it is still overwriting the file
// the user reviewed.
//
// A row whose files moved on writes nothing and is reported as failed with the reason stated, and
// contributes no import history, so a later import still offers it. The rows that still match run.
//
// A phone or camera a cable connects as a device rather than a drive has no path to scan, so Analyze first copies
// what its camera folders hold - less what an import already took - into a staging folder of the application's own,
// and analyzes that as it would a card; a file the device will not give up is named, and the rest staged. Import
// copies from a device whatever Move says, and the staged copies are removed once a run, the view, or another source
// is done with them.

#include "pch.h"
#include "model.h"
#include "model_db.h"
#include "model_index.h"
#include "view_import.h"

#include "app_command_status.h"
#include "app_commands.h"
#include "app_util.h"
#include "ui_dialog.h"

static import_options make_import_options()
{
	import_options result;
	result.source_filter = setting.import.source_filter;
	result.dest_folder = core::folder_path(setting.import.destination_path);
	result.dest_structure = setting.import.dest_folder_structure.empty() ? default_custom_folder_structure
	                                                                     : setting.import.dest_folder_structure;
	result.is_move = setting.import.is_move;
	result.collision = setting.import.collision;
	result.set_created_date = setting.import.set_created_date;
	result.rename_different_attributes = setting.import.rename_different_attributes;
	return result;
}

void import_view::invalidate_analysis()
{
	_analysis.clear();
	_analysis_valid = false;
	_rows.clear();
	_status.clear();
	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::status |
	                       view_invalid::command_state);
}

view_bars import_view::bars() const
{
	return task_view_bars({commands::import_analyze, commands::import_run});
}

view_controls_host_ptr import_view::controls(const ui::frame_ptr& owner)
{
	auto result = std::make_shared<view_controls_host>(_state);
	const auto& frame = owner;
	result->_controls = {
		std::make_shared<view_info_element>(icon_index::import, tt.command_import, tt.import_preparing)};
	result->_frame = frame;

	const std::weak_ptr<import_view> weak_view = shared_from_this();
	const std::weak_ptr<view_controls_host> weak_controls = result;
	auto selected_source = calc_import_selected_source(_state);

	_state.queue_async(async_queue::work,
	                   [&s = _state, weak_view, weak_controls, frame, selected_source = std::move(selected_source)]
	{
		std::vector<import_source> sources_temp;
		std::string error;
		try
		{
			sources_temp = calc_import_sources(selected_source);
		}
		catch (const std::exception& e)
		{
			error = core::utf8_cast(e.what());
		}

		s.queue_ui([weak_view, weak_controls, frame, sources_temp = std::move(sources_temp), error = std::move(error)]
		{
			const auto view = weak_view.lock();
			const auto controls = weak_controls.lock();
			if (!view || !controls) return;

			if (!error.empty())
			{
				controls->_controls = {std::make_shared<text_element>(error)};
				frame->layout();
				return;
			}

			view->_sources = sources_temp;
			view->populate_controls(controls, frame);
		});
	});

	return result;
}

void import_view::populate_controls(const view_controls_host_ptr& result, const ui::frame_ptr& frame)
{
	std::vector<view_element_ptr> controls;
	auto selection_thumbnails = std::make_shared<ui::selection_thumbnails_control>(frame);
	controls.emplace_back(std::make_shared<view_info_element>(icon_index::import, tt.command_import, tt.import_info));
	controls.emplace_back(selection_thumbnails);
	controls.emplace_back(std::make_shared<divider_element>());

	auto is_first_source = true;
	auto source_index = 0_z;
	std::weak_ptr<view_controls_host> weak_controls = result;

	controls.emplace_back(std::make_shared<text_element>(tt.import_from));

	for (auto&& src : _sources)
	{
		src.selected = is_first_source;
		if (is_first_source)
		{
			selection_thumbnails->selection_async(src.items.thumbs(), src.items.size(), _state._async);
		}
		auto check = std::make_shared<ui::check_control>(frame, src.text, src.selected, true, false,
		                                                 [this, selection_thumbnails, source_index,
		                                                  weak_controls](const bool checked)
		{
			invalidate_analysis();
			if (checked && source_index < _sources.size())
			{
				const auto& source_items = _sources[source_index].items;
				selection_thumbnails->selection_async(source_items.thumbs(), source_items.size(), _state._async);
				if (const auto controls = weak_controls.lock()) controls->scroll_controls();
			}
		},
		                                                 radio_group_scope);
		// A phone keeps its pictures: Import copies from one, whatever Move says.
		if (!src.device.id.empty()) check->child(std::make_shared<text_element>(tt.import_device_copies));
		controls.emplace_back(check);
		is_first_source = false;
		++source_index;
	}

	const std::vector<std::string> folder_structure_completes{std::string(default_custom_folder_structure),
	                                                          "{artist}\\{album}"s, "{show}\\Season {season}"s,
	                                                          "{year}\\{created}"s, "{year}\\{country}"s};

	_select_other_folder = is_first_source;
	auto limit = std::make_shared<ui::check_control>(frame, tt.import_other_folder, _select_other_folder, true, false,
	                                                 [this, selection_thumbnails, weak_controls](const bool checked)
	{
		invalidate_analysis();
		if (checked)
		{
			selection_thumbnails->selection({}, 0);
			if (const auto controls = weak_controls.lock()) controls->scroll_controls();
		}
	}, radio_group_scope);
	limit->child(std::make_shared<ui::folder_picker_control>(frame, setting.import.source_path, false,
	                                                         [this](const std::string_view)
	{ invalidate_analysis(); }));

	controls.emplace_back(limit);
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<text_element>(tt.import_dest_folder));
	controls.emplace_back(std::make_shared<ui::folder_picker_control>(frame, setting.import.destination_path, false,
	                                                                  [this](const std::string_view)
	{ invalidate_analysis(); }));
	controls.emplace_back(std::make_shared<text_element>(tt.import_dest_folder_structure));
	controls.emplace_back(std::make_shared<ui::edit_picker_control>(frame, setting.import.dest_folder_structure,
	                                                                folder_structure_completes,
	                                                                [this](const std::string_view)
	{ invalidate_analysis(); }));
	controls.emplace_back(set_margin(
		std::make_shared<link_element>(tt.more_template_information, [] { platform::open(doc_template_url); })));
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<ui::check_control>(frame, tt.import_ignore_previous,
	                                                          setting.import.ignore_previous, false, false,
	                                                          [this](const bool) { invalidate_analysis(); }));
	controls.emplace_back(
		create_collision_policy_control(frame, setting.import.collision, [this] { invalidate_analysis(); }));
	controls.emplace_back(std::make_shared<ui::check_control>(frame, tt.move_items, setting.import.is_move, false,
	                                                          false, [this](const bool) { invalidate_analysis(); }));
	controls.emplace_back(std::make_shared<ui::check_control>(frame, tt.import_set_created_date,
	                                                          setting.import.set_created_date, false, false,
	                                                          [this](const bool) { invalidate_analysis(); }));

	for (const auto& c : controls)
	{
		c->margin.cx = 8;
		c->margin.cy = 4;
	}

	result->_controls = controls;
	result->_frame = frame;
	result->populate();
	result->scroll_controls();
}

void import_view::update_rows(const import_analysis_result& analysis_result, const import_options& options)
{
	const auto gray_text_color = ui::style::tone_text(ui::style::tone::muted);
	const auto blue_text_color = ui::style::tone_text(ui::style::tone::accent);
	const auto orange_text_color = ui::style::tone_text(ui::style::tone::caution);

	std::vector<row_element_ptr> rows;
	std::vector<std::string> text;

	int imports = 0;
	int exists = 0;
	int already_imported = 0;
	int collisions = 0;
	// Rows are built in the order import_copy consumes the same analysis, so the count of importing
	// rows so far is the work index the run will report.
	int work_index = 0;

	for (const auto& a : analysis_result)
	{
		for (const auto& i : a.second)
		{
			auto row = std::make_shared<row_element>(*this);
			row->_text[1] = i.source.pack();
			row->_text[3] = i.destination.pack();

			if (i.already_exists) collisions += 1;

			// The Action column sorts by what each row does - imports, then what exists, then what was imported
			// before - rather than by how the action reads.
			switch (i.action)
			{
			case import_action::import:
				row->_text[0] = options.is_move ? tt.menu_move : tt.platform_text.menu_copy;

				// The destination already exists; name the policy that resolved it on the row itself
				// so an overwrite or a renamed destination is never silently implied.
				if (i.already_exists)
				{
					if (options.collision == collision_policy::replace)
					{
						row->_text[0] = std::format("{} ({})", row->_text[0], tt.collision_replace.sv());
						row->_text_color[0] = orange_text_color;
					}
					else if (options.collision == collision_policy::auto_rename)
					{
						row->_text[0] = std::format("{} ({})", row->_text[0], tt.collision_rename.sv());
						row->_text_color[0] = blue_text_color;
					}
					else
					{
						row->_text_color[0] = blue_text_color;
					}
				}
				else
				{
					row->_text_color[0] = blue_text_color;
				}

				row->_icons[2] = icon_index::next;
				row->_keys[0] = 1;
				imports += 1;
				break;
			case import_action::already_exists:
				row->_text_color[1] = gray_text_color;
				row->_text_color[3] = gray_text_color;
				row->_keys[0] = 2;
				row->_text[0] = tt.exists;
				exists += 1;
				break;
			case import_action::already_imported:
				row->_text_color[1] = gray_text_color;
				row->_text_color[3] = gray_text_color;
				row->_text_color[0] = orange_text_color;
				row->_text[0] = tt.previously_imported;
				row->_keys[0] = 3;
				already_imported += 1;
				break;
			}

			if (i.action == import_action::import) row->_work_index = work_index++;
			rows.emplace_back(row);
		}
	}

	review_rows(std::move(rows));
	_analysis = analysis_result;
	_analysis_options = options;
	_analysis_valid = true;
	// What a device still holds that an import already took was left on it rather than staged, so it has no row.
	_status = std::format("{} {}   {} {}   {} {}", imports, tt.import, exists, tt.exists,
	                      already_imported + _left_on_device, tt.previously_imported);

	// State the named policy that resolved the destination collisions rather than leaving it implicit.
	// Block Run and Skip report every collision, because that is what they answer. Replace and
	// Auto-rename report only the rows the plan will write, so the count matches what the run does and
	// what the confirmation about to follow it states.
	const auto collision_summary = format_collision_summary(options.collision,
	                                                        options.collision == collision_policy::replace ||
	                                                                options.collision == collision_policy::auto_rename
	                                                            ? static_cast<int>(
																	  count_import_colliding_writes(analysis_result))
	                                                            : collisions);

	if (!collision_summary.empty())
	{
		_status += "   ";
		_status += collision_summary;
	}

	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::status |
	                       view_invalid::command_state);
}

static auto calc_import_root(const std::vector<import_source>& sources, const bool select_other_folder)
{
	core::index_roots result;

	if (select_other_folder)
	{
		result.folders.emplace(core::folder_path(setting.import.source_path));
	}
	else
	{
		for (const auto& src : sources)
		{
			if (src.selected)
			{
				if (!src.path.is_empty())
				{
					result.folders.emplace(src.path);
				}

				for (const auto path : src.items.file_paths())
				{
					result.files.emplace(path);
				}

				for (const auto path : src.items.folder_paths())
				{
					result.folders.emplace(path);
				}
			}
		}
	}

	return result;
};

bool import_view::can_analyze() const
{
	if (setting.import.destination_path.empty()) return false;
	if (_select_other_folder) return !setting.import.source_path.empty();

	// Mirrors what calc_import_root would collect, without building the root set on the UI thread.
	return std::ranges::any_of(_sources, [](const import_source& src)
	{ return src.selected && (!src.path.is_empty() || !src.items.empty() || !src.device.id.empty()); });
}

std::optional<platform::portable_device_t> import_view::selected_device() const
{
	if (_select_other_folder) return {};

	for (const auto& src : _sources)
	{
		if (src.selected && !src.device.id.empty()) return src.device;
	}

	return {};
}

void import_view::show_progress(const size_t generation, std::string text)
{
	if (!is_processing_generation(generation)) return;
	_status = std::move(text);
	_state.invalidate_view(view_invalid::status);
}

// The staged copies are the device's files, not the user's: nothing is kept once the view no longer needs them.
void import_view::clear_staging()
{
	if (_staging.is_empty()) return;

	_state.queue_async(async_queue::work, [staging = std::exchange(_staging, {})]
	{
		std::error_code ec;
		std::filesystem::remove_all(platform::to_stream_path(staging), ec);
	});
	_left_on_device = 0;
}

void import_view::activate(const sizei extent)
{
	review_list::activate(extent);

	// Held while this instance runs, so another opening Import leaves what this one stages alone.
	if (!_staging_lock)
		_staging_lock = platform::try_lock_instance(device_staging_lock_name(platform::process_id())).lock;

	_state.queue_async(async_queue::work, [] { clear_abandoned_device_staging(device_staging_root()); });
}

void import_view::run()
{
	if (!can_run()) return;

	// Replace is the one policy here that destroys content, and unlike a delete it leaves nothing to
	// recover from. The review already marks each replacing row, so this states the total and its
	// permanence once, immediately before the run that acts on it.
	if (_analysis_options.collision == collision_policy::replace)
	{
		const auto replace_count = static_cast<int>(count_import_colliding_writes(_analysis));

		if (replace_count > 0)
		{
			const auto confirm = make_dlg(_host->owner());
			std::vector<view_element_ptr> controls;
			controls.emplace_back(
				set_margin(std::make_shared<ui::title_control2>(confirm->_frame, icon_index::import, tt.command_import,
			                                                    format_collision_summary(collision_policy::replace,
			                                                                             replace_count))));
			controls.emplace_back(set_margin(std::make_shared<text_element>(tt.import_replace_warning)));
			controls.emplace_back(std::make_shared<divider_element>());
			controls.emplace_back(std::make_shared<ui::ok_cancel_control>(confirm->_frame, tt.collision_replace));
			if (confirm->show_modal(controls) != ui::close_result::ok) return;
		}
	}

	record_feature_use(features::import);

	_state.recent_folders.add(setting.import.destination_path);
	// The plan and the options it was built from are moved out and marked invalid before any work
	// starts, so the reviewed rows are the only thing the worker can act on and a second Run cannot
	// reuse a plan already being run. After a run the user must Analyze again, which is also the
	// recovery path for any row that was refused.
	const auto analysis_result = std::move(_analysis);
	const auto options = _analysis_options;
	const auto completion_status = _status;
	_analysis_valid = false;
	const auto total = count_imports(analysis_result);
	begin_processing(total);
	const auto processing_generation = this->processing_generation();
	_run_generation = processing_generation;
	const auto cancel_source = processing_cancel_source();
	_status = std::string(tt.processing.sv());
	// Both made before the handles are released, which can pump messages: a Cancel - or a Close the
	// user confirms - handled in there has to reach this run. Made afterwards, each took the cancelled
	// count as its starting point and the copy went ahead regardless.
	auto token = core::cancel_token(*cancel_source);
	const auto view = shared_from_this();
	const auto results = make_run_status(view, processing_generation, cancel_source, completion_status);
	const auto detach = std::make_shared<detach_file_handles>(_state);

	_state.queue_async(async_queue::work, [&s = _state, results, view, analysis_result, options, token, cancel_source,
	                                       detach, processing_generation]
	{
		result_scope rr(results);
		const auto copy_result = import_copy(s.item_index, results, analysis_result, options, token);
		s._async.queue_database([copy_result](const database& db) { db.writes_item_imports(copy_result.imports); });

		// The token reads cancel_source without owning it, and this hop can outlive the
		// run status that otherwise keeps the source alive.
		s.queue_ui([&s, view, folder = copy_result.folder, created = copy_result.created, moved = copy_result.moved,
		            detach, processing_generation, token, cancel_source]() mutable
		{
			// The files arrived whatever became of the run's view, and what moved takes its earlier changes with it.
			s.move_changed(moved);
			s.record_change(icon_index::import, tt.command_import.sv(), std::move(created));
			// A device's staged copies have done their work, imported or not.
			view->clear_staging();

			// Navigation is the run's own conclusion, so a run the user cancelled or
			// replaced must not take the view somewhere when its worker finally
			// finishes - by then the user has moved on and would be pulled back.
			// Cancel stops the run without replacing it, so it is asked separately.
			if (token.is_cancelled() || !view->is_processing_generation(processing_generation)) return;

			if (!folder.is_empty())
			{
				detach->keep_display_closed();
				s.open(view->_host, df::search_t().add_selector(folder), {});
				s.invalidate_view(view_invalid::index);
			}
		});

		// import_copy completes the run itself, refused rows explained, and the first
		// completion is the only one shown. The scope above stands behind it for a
		// copy that throws instead.
	});
}

void import_view::analyze()
{
	auto import_root = calc_import_root(_sources, _select_other_folder);
	const auto device = selected_device();

	auto options = make_import_options();
	// A phone or camera keeps its pictures: what Import takes from one is copied, whatever Move says.
	if (device) options.is_move = false;

	if (options.dest_folder.is_empty())
	{
		_analysis.clear();
		_analysis_valid = false;
		_status = std::string(tt.platform_text.error_invalid_files.sv());
		_state.invalidate_view(view_invalid::status | view_invalid::command_state);
		return;
	}

	_analysis_valid = false;
	// What another device, or no device, was staged for is no use to this plan.
	if (const auto staging = device ? device_staging_folder(*device) : core::folder_path{}; staging != _staging)
	{
		clear_staging();
		_staging = staging;
	}
	_left_on_device = 0;
	begin_processing(0);
	const auto processing_generation = this->processing_generation();
	const auto cancel_source = processing_cancel_source();
	auto token = core::cancel_token(*cancel_source);
	_status = std::string(tt.analyzing.sv());

	// The token points into cancel_source without owning it, and the next Analyze replaces the view's
	// copy while this one may still be scanning, so the workers hold the source for as long as the token.
	_state._async.queue_database([&s = _state, import_root, device, staging = _staging, view = shared_from_this(),
	                              options, token, cancel_source, processing_generation](const database& db)
	{
		item_import_set previous_imported;
		try
		{
			previous_imported = setting.import.ignore_previous ? db.load_item_imports() : item_import_set{};
		}
		catch (const std::exception& e)
		{
			s.queue_ui([view, processing_generation, error = core::utf8_cast(e.what())]
			{
				if (!view->is_processing_generation(processing_generation)) return;
				view->end_processing();
				view->_analysis.clear();
				view->_analysis_valid = false;
				view->_status = error;
				view->_state.invalidate_view(view_invalid::status | view_invalid::command_state);
			});
			return;
		}

		s.queue_async(async_queue::work, [&s, import_root, device, staging, previous_imported, view, options, token,
		                                  cancel_source, processing_generation]() mutable
		{
			const auto fail = [&s, view, processing_generation](std::string error)
			{
				s.queue_ui([view, processing_generation, error = std::move(error)]
				{
					if (!view->is_processing_generation(processing_generation)) return;
					view->end_processing();
					view->_analysis.clear();
					view->_analysis_valid = false;
					view->_status = error;
					view->_state.invalidate_view(view_invalid::status | view_invalid::command_state);
				});
			};

			import_analysis_result analysis_result;
			size_t left_on_device = 0;
			std::vector<std::string> unreadable;
			try
			{
				if (device)
				{
					// A phone is read through the device rather than a path, so its files are staged where the
					// analysis can read them as it reads a card's.
					const auto name = device->name.empty() ? std::string(tt.import_device.sv()) : device->name;
					const auto say = [&s, view, processing_generation](std::string text)
					{
						s.queue_ui([view, processing_generation, text = std::move(text)]() mutable
						{ view->show_progress(processing_generation, std::move(text)); });
					};

					say(str_format(tt.import_reading_device_fmt.sv(), name));
					const auto files = platform::portable_device_files(device->id, device_media_folders, token);

					// A listing cut short by a cancel is not what the device holds, so nothing is staged from it.
					if (!token.is_cancelled())
					{
						if (files.empty())
						{
							fail(str_format(tt.import_device_empty_fmt.sv(), name));
							return;
						}

						const auto staged = stage_device_files(files, previous_imported, staging,
						                                       [&](const std::vector<platform::portable_copy_t>& copies)
						{
							const auto total = copies.size();
							say(str_format(tt.import_copying_device_fmt.sv(), name, 0, total));
							return platform::copy_from_portable_device(device->id, copies, token,
							                                           [&say, &name, total](const size_t tried)
							{ say(str_format(tt.import_copying_device_fmt.sv(), name, tried, total)); });
						});

						if (staged.short_of_space > 0)
						{
							fail(str_format(tt.import_device_no_space_fmt.sv(), name,
							                platform::format_size(core::file_size(staged.short_of_space))));
							return;
						}

						if (staged.stopped.failed() && !token.is_cancelled())
						{
							fail(staged.stopped.format_error());
							return;
						}

						left_on_device = staged.previously_imported;
						unreadable = staged.unreadable;
						(void)platform::create_folder(staging);
						import_root.folders.emplace(staging);
						say(std::string(tt.analyzing.sv()));
					}
				}

				const auto items = s.item_index.scan_items(import_root, true, true, token);
				analysis_result = import_analysis(items, options, previous_imported, token);
			}
			catch (const std::exception& e)
			{
				fail(std::string(core::utf8_cast(e.what())));
				return;
			}

			if (!token.is_cancelled())
			{
				s.queue_ui([analysis_result, options, view, processing_generation, left_on_device,
				            unreadable = std::move(unreadable)]
				{
					if (!view->is_processing_generation(processing_generation)) return;
					view->end_processing();
					view->_left_on_device = left_on_device;
					view->update_rows(analysis_result, options);

					// What the device would not give up has no row, so the status says it.
					if (!unreadable.empty())
					{
						view->_status += "   ";
						view->_status += format_plural_text(tt.import_device_unreadable_fmt, unreadable);
					}
				});
			}
			else
			{
				s.queue_ui([view, processing_generation]
				{
					if (!view->is_processing_generation(processing_generation)) return;
					view->end_processing();
					view->_analysis.clear();
					view->_analysis_valid = false;
					view->_status = std::string(tt.error_analysis_cancelled.sv());
					view->_state.invalidate_view(view_invalid::status | view_invalid::command_state);
				});
			}
		});
	});
}

void import_view::refresh()
{
	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::command_state);
}

void import_view::reload()
{
	// Refresh re-plans, which restarts an Analyze in progress. A Run in progress is left alone:
	// analyzing again would cancel it partway through without reporting what it had done.
	if (progress().active && is_processing_generation(_run_generation)) return;
	analyze();
}
