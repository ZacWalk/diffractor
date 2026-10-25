// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Command handlers for all user actions. Implements file operations, editing,
// sharing, metadata updates, and other application commands, and registers what each command does.
// The dialogs that are features in their own right are in app_dialogs.cpp.

#include "pch.h"

#include "util_top.h"
#include "model.h"
#include "model_index.h"
#include "model_db.h"
#include "model_tokenizer.h"
#include "ui_controls.h"
#include "ui_dialog.h"
#include "ui_controllers.h"
#include "view_edit.h"
#include "view_movie.h"
#include "view_items.h"
#include "app_match.h"
#include "app_text.h"
#include "app_command_status.h"
#include "app_util.h"
#include "app.h"
#include "view_import.h"
#include "view_locate.h"
#include "view_rename.h"
#include "view_batch.h"
#include "view_sync.h"
#include "view_tags.h"
#include "app_dialogs.h"

extern bool toggle_details_state;

bool can_process_selection_or_explain(const view_state& s, const view_host_base_ptr& view,
                                             const ui::control_frame_ptr& parent, const std::string_view title,
                                             const df::process_items_type type)
{
	const auto can_process = s.can_process_selection_and_mark_errors(view, type);
	if (!can_process.fail()) return true;

	make_dlg(parent)->show_message(icon_index::error, title, can_process.to_string());
	return false;
}

static void zoom_invoke(const view_state& s, const ui::control_frame_ptr& parent)
{
	const auto display = s.display_state();

	if (display && display->can_zoom())
	{
		display->toggle_zoom();
	}
};

static void zoom_fit_invoke(const view_state& s)
{
	if (const auto display = s.display_state(); display && display->can_zoom()) display->zoom(false);
}

static void zoom_fit_variant_invoke(const view_state& s, const df::zoom_scale_mode mode)
{
	if (const auto display = s.display_state(); display && display->can_zoom()) display->zoom_fit_variant(mode);
}

static void zoom_100_invoke(const view_state& s)
{
	if (const auto display = s.display_state(); display && display->can_zoom()) display->zoom_100();
}

static void zoom_toggle_fit_invoke(const view_state& s)
{
	if (const auto display = s.display_state(); display && display->can_zoom()) display->toggle_zoom_fit();
}

static void zoom_step_invoke(const view_state& s, const int direction)
{
	if (const auto display = s.display_state(); display && display->can_zoom()) display->adjust_zoom_scale(direction);
}

static void pin_invoke(view_state& s)
{
	// Only one item is ever held, so the command that reports a pin is held must also release it.
	// Re-pointing it at whatever has focus instead would leave no way back to plain browsing
	// without walking focus to the pinned item first.
	if (s.has_pin())
	{
		s._pin_item.reset();
	}
	else
	{
		s._pin_item = s.focus_item();
	}

	s.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw | view_invalid::command_state);
}


static void edit_invoke(view_state& s)
{
	if (s.can_edit_media())
	{
		s.view_mode(view_type::edit);
	}
}

static void containing_folder_invoke(view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view)
{
	const auto i = s.command_item();

	if (i)
	{
		df::unique_paths selection;
		selection.emplace(i->path());
		s.open(view, i->containing(), selection);
	}
}

static void open_in_file_browser_invoke(const view_state& s, const ui::control_frame_ptr& parent,
                                        const view_host_base_ptr& view)
{
	if (can_process_selection_or_explain(s, view, parent, tt.open_in_browser_title,
	                                     df::process_items_type::local_file_or_folder))
	{
		const auto f = s.command_item();

		if (f)
		{
			const auto file = std::dynamic_pointer_cast<df::item_element>(f);

			if (file)
			{
				platform::show_in_file_browser(file->path());
			}
		}
	}
}

static void new_folder_invoke(view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view)
{
	if (!s.search().is_showing_folder()) return;

	int i = 2;
	const auto folder = s.save_path();
	auto new_name = std::string(tt.new_folder_name);
	auto new_path = folder.combine(new_name);

	while (platform::exists(new_path) && i < 100)
	{
		new_name = std::format("{} {}", tt.new_folder_name, i++);
		new_path = folder.combine(new_name);
	}

	const auto create_folder_result = platform::create_folder(new_path);

	if (create_folder_result.failed())
	{
		const auto dlg = make_dlg(parent);
		dlg->show_message(icon_index::folder, tt.new_folder_title, create_folder_result.format_error());
	}
	else
	{
		df::unique_paths selection;
		selection.emplace(new_path);
		s.open(view, s.search(), selection);
	}
}

static void burn_command_invoke(view_state& s, const ui::control_frame_ptr& parent,
                                const view_host_base_ptr& view)
{
	const auto dlg = make_dlg(parent);
	const auto title = tt.burn_title;

	pause_media pause(s);

	if (can_process_selection_or_explain(s, view, parent, title, df::process_items_type::local_file_or_folder))
	{
		const auto items = s.selected_items();
		auto title_control = std::make_shared<ui::title_control2>(
			dlg->_frame, icon_index::disk, title, format_plural_text(tt.burn_info_fmt, items),
			std::vector<ui::const_surface_ptr>{}, items.size());
		title_control->selection_async(items.thumbs(), items.size(), s._async);
		const std::vector<view_element_ptr> controls = {
			set_margin(title_control),
			std::make_shared<divider_element>(),
			set_margin(std::make_shared<text_element>(tt.burn_help)),
			std::make_shared<divider_element>(),
			std::make_shared<ui::ok_cancel_control>(dlg->_frame, tt.burn_stage),
		};

		if (dlg->show_modal(controls) == ui::close_result::ok)
		{
			record_feature_use(features::burn_to_disk);

			if (!platform::burn_to_cd(items.file_paths(true), items.folder_paths()))
			{
				dlg->show_message(icon_index::error, title, tt.burn_failed);
			}
		}
	}
}

static void print_invoke(const view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view)
{
	if (can_process_selection_or_explain(s, view, parent, tt.print_title, df::process_items_type::local_file))
	{
		const auto items = s.selected_items();
		record_feature_use(features::print);
		platform::print(items.file_paths(false), items.folder_paths());
	}
}

quadd rotate_command_crop_bounds(const sizei dimensions, const ui::orientation stored_orientation,
                                 const simple_transform transform, const bool show_rotated)
{
	const auto current_orientation = show_rotated ? stored_orientation : ui::orientation::top_left;
	return quadd(dimensions).transform(to_simple_transform_inv(current_orientation)).transform(transform);
}

static void rename_invoke(view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view)
{
	const auto title = tt.command_rename;
	constexpr auto icon = icon_index::rename;
	const auto dlg = make_dlg(parent);

	pause_media pause(s);

	if (can_process_selection_or_explain(s, view, parent, title, df::process_items_type::local_file_or_folder))
	{
		const auto& items = s.selected_items();

		if (items.size() > 1)
		{
			s.view_mode(view_type::rename);
		}
		else if (items.size() == 1)
		{
			const auto& file_system_items = items.items();
			const auto i = file_system_items[0];
			auto name = i->base_name();
			auto title_control = std::make_shared<ui::title_control2>(
				dlg->_frame, icon, title, format_plural_text(tt.rename_fmt, items),
				std::vector<ui::const_surface_ptr>{}, items.size());
			title_control->selection_async(items.thumbs(), items.size(), s._async);

			const std::vector<view_element_ptr> controls{
				set_margin(title_control),
				std::make_shared<divider_element>(),
				set_margin(std::make_shared<text_element>(tt.rename_label)),
				set_margin(std::make_shared<ui::edit_control>(dlg->_frame, name)),
				std::make_shared<divider_element>(),
				std::make_shared<ui::ok_cancel_control>(dlg->_frame)
			};

			if (dlg->show_modal(controls) == ui::close_result::ok)
			{
				detach_file_handles detach(s);

				const auto result = i->rename(s.item_index, name);

				if (result.failed())
				{
					dlg->show_message(icon_index::error, title, result.format_error(tt.error_rename_failed, {}));
				}
			}
		}

		s.invalidate_view(
			view_invalid::view_layout | view_invalid::group_layout | view_invalid::media_elements |
			view_invalid::app_layout);
	}
}

static void file_properties_invoke(const view_state& s, const ui::control_frame_ptr& parent,
                                   const view_host_base_ptr& view)
{
	if (can_process_selection_or_explain(s, view, parent, tt.open_properties_title,
	                                     df::process_items_type::local_file_or_folder))
	{
		const auto items = s.selected_items();
		platform::show_file_properties(items.file_paths(false), items.folder_paths());
	}
}

static void edit_paste_invoke(view_state& s, const ui::control_frame_ptr& parent,
                              const std::shared_ptr<view_frame>& view)
{
	const auto dlg = make_dlg(parent);
	const auto data = platform::clipboard();
	platform::file_op_result result;

	if (data->has_drop_files())
	{
		detach_file_handles detach(s);
		shell_file_operation_ui processing(*view, parent);
		result = data->drop_files(s.save_path(), platform::drop_effect::none);
		if (result.success() && (!result.created_files.files.empty() || !result.created_files.folders.empty()))
			detach.keep_display_closed();
	}
	else if (data->has_bitmap())
	{
		auto bitmap = ui_owned(s._async, data->capture_bitmap());
		if (!bitmap)
		{
			result.error_message = tt.platform_text.error_save_image;
		}
		else
		{
			const auto save_path = s.save_path();
			const auto file_name = std::string(tt.pasted_file_name);
			const auto results = std::make_shared<command_status>(s._async, dlg, icon_index::edit_paste,
			                                                       tt.command_edit_paste, 1);

			s.queue_async(async_queue::work,
			              [&s, view = ui_owned(s._async, view), results, bitmap = std::move(bitmap), save_path,
				              file_name]
			{
				auto save_result = bitmap->save(save_path, file_name, true);

				if (save_result.success())
				{
					s.queue_ui([&s, view, results, created_files = std::move(save_result.created_files)]
					{
						results->complete();
						if (!created_files.files.empty() || !created_files.folders.empty())
						{
							detach_file_handles detach(s);
							detach.keep_display_closed();
							s.open(view.shared(), s.search(), make_unique_paths(created_files));
						}
					});
				}
				else
				{
					results->abort(str::is_empty(save_result.error_message)
						               ? std::string(tt.platform_text.error_unknown)
						               : save_result.error_message);
				}
			});

			results->wait_for_complete();
			return;
		}
	}

	if (result.failed())
	{
		dlg->show_message(icon_index::error, tt.command_edit_paste,
		                  str::is_empty(result.error_message) ? tt.platform_text.error_unknown.sv() : result.error_message);
	}
	else
	{
		s.open(view, s.search(), make_unique_paths(result.created_files));
	}
}

static void show_flatten_invoke(view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view)
{
	const auto current_search = s.search();

	if (current_search.has_selector())
	{
		const auto first_selector = current_search.selectors().front();

		if (first_selector.is_recursive())
		{
			auto a = current_search;
			a.remove_recursive();
			s.open(view, a, {});
		}
		else
		{
			auto a = current_search;
			a.set_recursive(true);
			s.open(view, a, {});
		}
	}
}

static void setting_invoke(const view_state& s, bool& val, const bool new_val)
{
	val = new_val;
	s.invalidate_view(view_invalid::view_layout | view_invalid::group_layout | view_invalid::app_layout);
}

static void zoom_navigator_mode_invoke(const view_state& state, const zoom_navigator_mode mode)
{
	setting.zoom_navigator = mode;
	if (const auto display = state.display_state()) display->mark_zoom_activity();
	state.invalidate_view(view_invalid::view_redraw | view_invalid::controller | view_invalid::options_save);
}

static void rotate_invoke(view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view,
                          const simple_transform t)
{
	// Rotation rewrites each item in place, so it never needs a save folder. The only
	// precondition is that the selection can be written back, which can_process checks below
	// and which is also what enables the command.
	auto dlg = make_dlg(parent);
	const auto title = t == simple_transform::rot_90 ? tt.command_rotate_clockwise : tt.command_rotate_anticlockwise;
	const auto icon = t == simple_transform::rot_90 ? icon_index::rotate_clockwise : icon_index::rotate_anticlockwise;

	pause_media pause(s);

	if (can_process_selection_or_explain(s, view, parent, title, df::process_items_type::can_save_pixels))
	{
		const auto& items = s.selected_items();
		const auto is_single = items.size() == 1;

		// A single-item rotation is reviewed only while the user asks for it; rotating several items
		// overwrites several originals at once and is always reviewed.
		if (!is_single || setting.confirm_rotations)
		{
			const auto surfaces = items.thumbnail_surfaces(1);
			bool confirm_single = setting.confirm_rotations;
			auto title_control = std::make_shared<ui::title_control2>(
				dlg->_frame, icon, title, format_plural_text(tt.rotate_info_fmt, items),
				std::vector<ui::const_surface_ptr>{}, items.size());
			title_control->selection_async(items.thumbs(), items.size(), s._async);

			std::vector<view_element_ptr> controls;
			controls.emplace_back(set_margin(title_control));
			controls.emplace_back(std::make_shared<divider_element>());

			if (!surfaces.empty())
			{
				controls.emplace_back(std::make_shared<ui::before_after_control>(surfaces.front(), t));
			}

			if (is_single)
			{
				controls.emplace_back(set_margin(std::make_shared<ui::check_control>(
					dlg->_frame, tt.rotate_confirm_single, confirm_single)));
			}

			controls.emplace_back(std::make_shared<divider_element>());
			controls.emplace_back(std::make_shared<ui::ok_cancel_control>(dlg->_frame, tt.button_rotate));

			if (dlg->show_modal(controls) != ui::close_result::ok) return;

			if (is_single && confirm_single != setting.confirm_rotations)
			{
				setting.confirm_rotations = confirm_single;
				s.invalidate_view(view_invalid::options_save);
			}
		}

		{
			record_feature_use(features::rotate);

			const auto results = std::make_shared<command_status>(s._async, dlg, icon, title, items.size());

			batch_edit_spec spec;
			spec.process_type = df::process_items_type::can_save_pixels;
			spec.encode_params = make_file_encode_params();
			// A rotation always changes what is drawn, whether it is stored as an orientation tag or
			// as rotated pixels.
			spec.changes_presentation = true;
			spec.make_edits = [t](const df::file_path path) -> std::optional<item_edits>
			{
				// Only the frame size and stored orientation are needed, and load_image_file derives
				// both from this same scan, so read the header instead of the whole file.
				sizei dimensions{0, 0};
				auto stored_orientation = ui::orientation::top_left;
				file_read_stream stream;

				if (stream.open(path))
				{
					const auto info = scan_photo(stream);
					dimensions = info.dimensions();
					stored_orientation = info.orientation;
				}

				if (dimensions.cx <= 0 || dimensions.cy <= 0)
				{
					// Bytes the header scan cannot describe, such as a container carrying a saveable
					// extension; the full load still decodes them.
					files ff;
					const auto load_result = ff.load(path, false);
					if (!load_result.success) return {};
					dimensions = load_result.dimensions();
					stored_orientation = load_result.orientation();
				}

				item_edits result;
				const auto current_orientation = setting.show_rotated
					                                 ? stored_orientation
					                                 : ui::orientation::top_left;
				const auto crop = rotate_command_crop_bounds(dimensions, stored_orientation, t, setting.show_rotated);

				if (current_orientation != ui::orientation::top_left)
				{
					result.metadata.orientation = ui::orientation::top_left;
				}

				result.image.crop_bounds(crop);
				return result;
			};

			s.modify_items(results, items.items(), spec, view);
		}
	}
}

static void desktop_background_invoke(view_state& s, const ui::control_frame_ptr& parent)
{
	s.capture_display([&s, parent](const file_load_result& loaded)
	{
		const auto title = tt.command_desktop_background;
		auto dlg = make_dlg(parent);
		pause_media pause(s);
		const std::vector<view_element_ptr> controls = {
			set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon_index::wallpaper, title,
			                                                tt.desktop_background_info)),
			std::make_shared<divider_element>(),
			std::make_shared<wallpaper_control>(dlg->_frame, loaded, setting.desktop_background.maximize),
			set_margin(std::make_shared<ui::check_control>(dlg->_frame, tt.maximize_image,
			                                               setting.desktop_background.maximize, false, false,
			                                               [d = dlg->_frame](bool checked) { d->invalidate(); })),
			std::make_shared<divider_element>(),
			std::make_shared<ui::ok_cancel_control>(dlg->_frame),
		};

		if (dlg->show_modal(controls) == ui::close_result::ok)
		{
			constexpr auto write_extension = ".png";
			const auto write_path = df::file_path(known_path(platform::known_folder::app_data), "wallpaper",
			                                      write_extension);
			const auto path_temp = platform::temp_file(write_extension);
			const auto bounds = ui::desktop_bounds(true);
			const auto screen_extent = bounds.extent();
			const auto max_dim = std::max(screen_extent.cx, screen_extent.cy);
			const auto dimensions = setting.desktop_background.maximize ? sizei(max_dim, max_dim) : screen_extent;
			const auto encode_params = make_file_encode_params();

			const auto results = std::make_shared<command_status>(s._async, dlg, icon_index::wallpaper, title, 1);

			// Writing a screen-sized PNG twice is far too slow to run on the UI thread. Only the
			// wallpaper switch comes back, and a failure is now reported instead of being
			// described as a failed update.
			s.queue_async(async_queue::work,
			              [&s, results, loaded, write_path, path_temp, dimensions, encode_params]
			              {
				              files ff;
				              const image_edits edits(dimensions);
				              std::string error;

				              if (!ff.save(path_temp, loaded))
				              {
					              error = str_format(tt.error_create_file_failed_fmt.sv(), path_temp);
				              }
				              else
				              {
					              const auto update_result = ff.update(path_temp, write_path, {}, edits, encode_params,
					                                                   false, {});

					              if (update_result.failed())
					              {
						              error = update_result.format_error(
							              str_format(tt.error_create_file_failed_fmt.sv(), write_path));
					              }
				              }

				              if (path_temp.exists())
				              {
					              platform::delete_file(path_temp);
				              }

				              if (error.empty())
				              {
					              s.queue_ui([results, write_path]
					              {
						              platform::set_desktop_wallpaper(write_path);
						              results->complete();
					              });
				              }
				              else
				              {
					              results->abort(error);
				              }
			              });

			results->wait_for_complete();
		}
	});
}

static void capture_invoke(view_state& s, const ui::control_frame_ptr& parent)
{
	// Read now, not when the frame arrives: the frame is of this item, so this is the name it has
	// to be offered under. Reading it in the callback named the capture after whatever the display
	// had moved on to. Only the path is kept - the callback waits on the media read thread, which
	// must not end up holding the item itself.
	const auto item = s.command_item();

	if (!item) return;

	s.capture_display([&s, parent, source_path = item->path()](const file_load_result& lr)
	{
		if (!lr.is_empty())
		{
			auto i = 1;
			const auto save_ext = lr.is_jpeg() ? "jpg" : "png";
			const auto save_folder = s.save_path();
			const auto save_name = source_path.file_name_without_extension();
			auto save_path = df::file_path(save_folder, save_name, save_ext);

			while (save_path.exists())
			{
				save_path = df::file_path(save_folder, std::format("{}-{}", save_name, i++), save_ext);
			}

			if (platform::prompt_for_save_path(save_path))
			{
				files ff;

				if (!ff.save(save_path, lr))
				{
					const auto dlg = make_dlg(parent);
					dlg->show_message(icon_index::error, tt.command_capture, tt.platform_text.error_save_image);
				}
			}
		}
	});
}

static void related_invoke(view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view)
{
	const auto i = s.command_item();

	if (i)
	{
		df::related_info r;
		r.load(i);
		s.open(view, df::search_t().related(r), {});
	}
}

static void favorite_tags_invoke(view_state& s, const ui::control_frame_ptr& parent)
{
	auto dlg = make_dlg(parent);
	auto favorite_tags = setting.favorite_tags;
	auto edit = std::make_shared<ui::multi_line_edit_control>(dlg->_frame, favorite_tags);
	auto content = std::make_shared<ui::group_control>();
	const auto toolbar_refresh = std::make_shared<ui::recommended_words_control::refresh_group::element_type>();
	const auto unique_favorites = [&favorite_tags]
	{
		std::vector<std::string> result;
		df::hash_set<std::string, df::ihash, df::ieq> seen;
		for (const auto tag : str::split(favorite_tags, true))
		{
			if (seen.emplace(tag).second) result.emplace_back(tag);
		}
		return result;
	};
	const auto contains_favorite = [&unique_favorites](const std::string_view tag)
	{
		const auto favorites = unique_favorites();
		return std::ranges::find_if(favorites, [tag](const std::string_view favorite)
		{
			return str::icmp(favorite, tag) == 0;
		}) != favorites.end();
	};
	const auto toggle_favorite = [&favorite_tags, edit, unique_favorites](const std::string_view tag)
	{
		auto favorites = unique_favorites();
		const auto found = std::ranges::find_if(favorites, [tag](const std::string_view favorite)
		{
			return str::icmp(favorite, tag) == 0;
		});
		if (found == favorites.end())
			favorites.emplace_back(tag);
		else
			favorites.erase(found);
		favorite_tags = str::combine(favorites);
		edit->text(favorite_tags);
	};
	favorite_tags = str::combine(unique_favorites());
	edit->text(favorite_tags);

	content->add(set_margin(std::make_shared<text_element>(tt.customise_tags_help)));
	content->add(set_margin(std::make_shared<text_element>(tt.help_tag1)));
	content->add(set_margin(std::make_shared<text_element>(tt.help_tag2)));
	content->add(edit);

	df::string_counts common_counts;
	for (const auto& tag : s.item_index.distinct_tags())
	{
		++common_counts[tag.first];
	}
	s.recent_tags.count_strings(common_counts, 1000000);

	const auto common_tags = top_map(common_counts, 20);
	if (!common_tags.empty())
	{
		content->add(std::make_shared<ui::title_control>(tt.tags_common_label));
		content->add(set_margin(std::make_shared<ui::recommended_words_control>(
			dlg->_frame, common_tags, toggle_favorite, contains_favorite, toolbar_refresh)));
	}

	static constexpr std::array<std::string_view, 12> workflow_tags = {
		"Approved", "Archive", "Client", "Draft", "Final", "Progress", "Review", "Published",
		"Rejected", "Rights", "Select", "Todo"
	};
	std::vector<std::string_view> workflow_suggestions;
	df::hash_set<std::string_view, df::ihash, df::ieq> workflow_seen;
	const auto append_workflow_tag = [&workflow_suggestions, &workflow_seen](const std::string_view tag)
	{
		if (workflow_seen.emplace(tag).second) workflow_suggestions.emplace_back(tag);
	};
	for (const auto tag : str::split(tt.default_favorite_tags, true)) append_workflow_tag(tag);
	for (const auto tag : workflow_tags) append_workflow_tag(tag);
	content->add(std::make_shared<ui::title_control>(tt.tags_workflow_label));
	content->add(set_margin(std::make_shared<ui::recommended_words_control>(
		dlg->_frame, workflow_suggestions, toggle_favorite, contains_favorite, toolbar_refresh)));

	std::vector<view_element_ptr> controls;
	controls.emplace_back(set_margin(std::make_shared<ui::title_control2>(
		dlg->_frame, icon_index::tag, tt.customise_tags_title, tt.customise_tags_help)));
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(content);
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<ui::ok_cancel_control>(dlg->_frame));

	pause_media pause(s);

	// The edit box and the suggestion toolbars all write to the local copy, so Cancel and
	// Escape leave the saved favourites alone.
	if (dlg->show_modal(controls, {54}, {66}) != ui::close_result::ok) return;

	favorite_tags = str::combine(unique_favorites());
	setting.favorite_tags = favorite_tags;
	s.invalidate_view(view_invalid::sidebar | view_invalid::options_save | view_invalid::command_state |
		view_invalid::tooltip);
}

static void rate_items_invoke(view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view, int r)
{
	auto dlg = make_dlg(parent);
	metadata_edits edits;
	edits.rating = r;

	constexpr auto icon = icon_index::star;
	const auto title = tt.prop_name_rating;
	const auto results = std::make_shared<command_status>(s._async, dlg, icon, title, s.selected_count());

	s.modify_items(results, icon, title, s.selected_items().items(), edits, view);
}

static void label_items_invoke(view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view,
                               const std::string_view label)
{
	const auto selected = s.selected_items();
	const auto& items = selected.items();

	// Applying a label the whole selection already carries removes it, so a key repeats the toggle
	// the grading control performs with the pointer and clearing never needs a separate command.
	const auto is_set = !label.empty() && !items.empty() &&
		std::ranges::all_of(items, [label](const df::item_element_ptr& i)
		{
			return str::icmp(i->label(), label) == 0;
		});

	auto dlg = make_dlg(parent);
	metadata_edits edits;
	edits.label = is_set ? std::string_view{} : label;

	constexpr auto icon = icon_index::flag;
	const auto title = tt.prop_name_label;
	const auto results = std::make_shared<command_status>(s._async, dlg, icon, title, items.size());

	s.modify_items(results, icon, title, items, edits, view);
}

static void cut_copy_invoke(const view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view,
                            const bool is_move)
{
	const auto title = is_move ? tt.command_edit_cut : tt.command_edit_copy;

	if (can_process_selection_or_explain(s, view, parent, title, df::process_items_type::local_file_or_folder))
	{
		const auto items = s.selected_items();
		const auto item_paths = items.file_paths(true);
		const auto folder_paths = items.folder_paths();

		s.capture_display([item_paths, folder_paths, is_move = is_move](const file_load_result& loaded)
		{
			platform::set_clipboard(item_paths, folder_paths, clipboard_image_of(loaded), is_move);
		});
	}
}

static void copy_item_path_invoke(const view_state& s)
{
	const auto items = s.selected_items().items();
	std::string paths;

	for (const auto& item : items)
	{
		if (!paths.empty()) paths += "\r\n";
		paths += item->path().pack();
	}

	if (!paths.empty()) platform::set_clipboard(paths);
}

class folder_auto_complete final : public std::enable_shared_from_this<folder_auto_complete>,
                                   public ui::complete_strategy_t
{
	view_state& _state;
	std::shared_ptr<const df::folder_counts> _folders;
	std::shared_ptr<std::atomic_uint64_t> _latest_request = std::make_shared<std::atomic_uint64_t>(0);
	destination_folder_enumerator _enumerate_child_folders;
	std::vector<df::folder_path> _recents;
	ui::control_frame_ptr _parent;
	ui::auto_complete_match_ptr _selected;
	uint64_t _request_id = 0;
	std::weak_ptr<void> _picker_lifetime;

public:
	std::string no_results_message() override
	{
		return std::string(tt.type_to_search);
	}

	std::weak_ptr<ui::search_control> _search_control;


	folder_auto_complete(view_state& s, ui::control_frame_ptr parent,
	                     std::shared_ptr<void> picker_lifetime = {},
	                     std::shared_ptr<const df::folder_counts> folders = {},
	                     destination_folder_enumerator enumerate_child_folders = {}) :
		_state(s),
		_folders(folders ? std::move(folders) : std::make_shared<df::folder_counts>(_state.known_folders())),
		_enumerate_child_folders(std::move(enumerate_child_folders)),
		_parent(std::move(parent)),
		_picker_lifetime(std::move(picker_lifetime))
	{
		resize_to_show_results = false;
		auto_select_first = false;
		folder_select_button = true;
		max_predictions = 12u;

		if (!_enumerate_child_folders)
		{
			_enumerate_child_folders = [](const df::folder_path folder)
			{
				df::folder_paths result;
				const df::item_selector selector(folder);

				for (const auto& fi : platform::select_folders(selector, setting.show_hidden))
				{
					result.emplace_back(folder.combine(fi.name));
				}

				return result;
			};
		}

		auto recents = _state.recent_folders.items();
		std::ranges::reverse(recents);

		if (recents.size() > max_predictions)
		{
			recents.resize(max_predictions);
		}

		for (const auto& i : recents)
		{
			_recents.emplace_back(df::folder_path(i));
		}
	}

	void bind_search_control(const std::shared_ptr<ui::search_control>& search_control)
	{
		_search_control = search_control;
		_picker_lifetime = search_control;
	}

	void initialise(std::function<void(const ui::auto_complete_results&)> complete) override
	{
		search({}, std::move(complete));
	}

	static bool compare_weight(const ui::auto_complete_match_ptr& l, const ui::auto_complete_match_ptr& r)
	{
		const auto diff = l->weight - r->weight;
		return diff == 0 ? str::icmp(l->edit_text(), r->edit_text()) < 0 : diff > 0;
	}

	struct folder_completion_candidate
	{
		df::folder_path folder;
		ui::match_highlights match;
		int weight = 0;
	};

	static void add_candidate(df::hash_map<df::folder_path, folder_completion_candidate, df::ihash, df::ieq>& results,
	                          const df::folder_path folder, ui::match_highlights m, const int weight)
	{
		const auto found = results.find(folder);

		if (found != results.end())
		{
			found->second.weight += weight;
			return;
		}

		results.emplace(folder, folder_completion_candidate{folder, std::move(m), weight});
	}

	static std::vector<folder_completion_candidate> find_matches(const std::string& query,
	                                                             const df::folder_counts& folders,
	                                                             const destination_folder_enumerator& enumerate_child_folders,
	                                                             const size_t max_results,
	                                                             const std::function<bool()>& is_stale = {})
	{
		df::hash_map<df::folder_path, folder_completion_candidate, df::ihash, df::ieq> candidates;
		const auto query_parts = str::split(query, true);

		for (const auto& folder : folders)
		{
			ui::match_highlights m;

			if (find_auto_complete(query_parts, folder.first.text(), true, m))
			{
				add_candidate(candidates, folder.first, std::move(m), folder.second);
			}
		}

		if (is_stale && is_stale()) return {};

		if (df::is_path(query))
		{
			const df::folder_path folder(query);

			for (const auto& path : enumerate_child_folders(folder))
			{
				ui::match_highlights m;

				if (find_auto_complete(query_parts, path.text(), true, m))
				{
					const auto known = folders.find(path);
					const auto known_weight = candidates.contains(path) || known == folders.end() ? 0 : known->second;
					add_candidate(candidates, path, std::move(m), 10 + known_weight);
				}
			}
		}

		std::vector<folder_completion_candidate> results;
		results.reserve(candidates.size());
		for (auto&& candidate : candidates) results.emplace_back(std::move(candidate.second));

		std::ranges::sort(results, [](const auto& l, const auto& r)
		{
			const auto diff = l.weight - r.weight;
			return diff == 0 ? str::icmp(l.folder.text(), r.folder.text()) < 0 : diff > 0;
		});

		if (results.size() > max_results) results.resize(max_results);
		return results;
	}

	void search(const std::string& query,
	            const std::function<void(const ui::auto_complete_results&)> complete) override
	{
		df::assert_true(ui::is_ui_thread());
		const auto request_id = ++_request_id;
		_latest_request->store(request_id, std::memory_order_release);

		if (query.empty())
		{
			ui::auto_complete_results results;

			for (const auto& folder : _recents)
			{
				results.emplace_back(std::make_shared<folder_match>(*this, folder, ui::match_highlights{}, 1, true));
			}

			complete(results);
		}
		else
		{
			auto folders = _folders;
			auto latest_request = _latest_request;
			auto enumerate_child_folders = _enumerate_child_folders;
			const auto max_results = max_predictions;
			auto weak = weak_from_this();
			auto complete_ui = ui_owned(
				_state._async, std::make_shared<std::function<void(const ui::auto_complete_results&)>>(complete));

			_state.queue_async(async_queue::work,
			                   [&s = _state, weak, query, folders = std::move(folders),
				                   enumerate_child_folders = std::move(enumerate_child_folders), latest_request,
				                   max_results, request_id, complete_ui]
			                   {
				                   const auto is_stale = [latest_request, request_id]
				                   {
					                   return latest_request->load(std::memory_order_acquire) != request_id;
				                   };

				                   if (is_stale()) return;

				                   auto found = find_matches(query, *folders, enumerate_child_folders, max_results,
				                                             is_stale);

				                   s.queue_ui([weak, found = std::move(found), request_id, complete_ui]() mutable
				                   {
					                   const auto self = weak.lock();
					                   if (!self || self->_request_id != request_id || self->_picker_lifetime.expired())
					                   {
						                   return;
					                   }

					                   ui::auto_complete_results results;
					                   results.reserve(found.size());

					                   for (auto& candidate : found)
					                   {
						                   results.emplace_back(std::make_shared<folder_match>(
							                   *self, candidate.folder, std::move(candidate.match), candidate.weight, true));
					                   }

					                   (*complete_ui)(results);
				                   });
			                   });
		}
	}

	void selected(const ui::auto_complete_match_ptr& i, const select_type st) override
	{
		_selected = i;

		const auto c = _search_control.lock();

		if (c && i)
		{
			c->update_edit_text(i->edit_text());
		}
	}

	ui::auto_complete_match_ptr selected() const override
	{
		return _selected;
	}
};

std::shared_ptr<ui::complete_strategy_t> make_destination_folder_auto_complete_for_test(
	view_state& state, std::shared_ptr<void> picker_lifetime, std::shared_ptr<const df::folder_counts> folders,
	destination_folder_enumerator enumerate_child_folders)
{
	return std::make_shared<folder_auto_complete>(state, nullptr, std::move(picker_lifetime), std::move(folders),
	                                             std::move(enumerate_child_folders));
}

df::file_path selected_external_tool_target(const df::item_set& selected_items)
{
	if (selected_items.size() != 1 || selected_items.has_folders()) return {};
	return selected_items.items().front()->path();
}

static void copy_move_invoke(view_state& s, const ui::control_frame_ptr& parent,
                             const std::shared_ptr<view_frame>& view,
                             const bool is_move)
{
	const auto title = is_move ? tt.command_move : tt.command_copy;
	const auto icon = is_move ? icon_index::move_to_folder : icon_index::copy_to_folder;
	auto dlg = make_dlg(parent);

	pause_media pause(s);

	if (can_process_selection_or_explain(s, view, parent, title, df::process_items_type::local_file_or_folder))
	{
		// Opening with an empty box means OK fails on the state the dialog itself started in,
		// so it starts on the destination last used.
		std::string text = s.recent_folders.items().empty() ? std::string{} : s.recent_folders.items().back();

		const auto auto_complete = std::make_shared<folder_auto_complete>(s, parent);
		const auto search_control = std::make_shared<ui::search_control>(dlg->_frame, text, auto_complete);
		auto_complete->bind_search_control(search_control);

		const auto& items = s.selected_items();
		const auto items_message = format_plural_text(is_move ? tt.move_fmt : tt.copy_fmt, items);
		auto title_control = std::make_shared<ui::title_control2>(
			dlg->_frame, icon, title, items_message, std::vector<ui::const_surface_ptr>{}, items.size());
		title_control->selection_async(items.thumbs(), items.size(), s._async);

		std::vector<view_element_ptr> controls = {
			set_margin(title_control),
			std::make_shared<divider_element>(),
			search_control,
			std::make_shared<ui::check_control>(dlg->_frame, tt.open_dest, setting.show_results),
			std::make_shared<divider_element>(),
			std::make_shared<ui::ok_cancel_control>(dlg->_frame)
		};

		if (ui::close_result::ok == dlg->show_modal(controls, {66}))
		{
			df::folder_path write_folder(text);

			if (!write_folder.is_qualified())
			{
				dlg->show_message(icon_index::error, title, str_format(tt.is_not_valid_folder_fmt.sv(), write_folder));
			}
			else if (const auto create_folder_result = platform::create_folder(write_folder); create_folder_result.
				failed())
			{
				dlg->show_message(icon_index::error, title,
				                  create_folder_result.format_error(
					                  str_format(tt.failed_to_create_folder_fmt.sv(), write_folder)));
			}
			else
			{
				// The shell would silently auto-rename every collision, so a user who meant to replace
				// got a second copy and no way to say otherwise. Name the collisions and let them
				// choose, defaulting to the option that cannot destroy anything.
				auto replace_existing = false;
				const auto collisions = check_overwrite(write_folder, items, {});

				if (!collisions.empty())
				{
					const auto collision_dlg = make_dlg(parent);
					auto policy = collision_policy::auto_rename;

					std::vector<view_element_ptr> collision_controls;
					collision_controls.emplace_back(set_margin(std::make_shared<ui::title_control2>(
						collision_dlg->_frame, icon, title,
						format_plural_text(tt.would_overwrite_fmt, collisions))));
					collision_controls.emplace_back(create_collision_policy_control(
						collision_dlg->_frame, policy, [] {}, true, false));
					collision_controls.emplace_back(std::make_shared<divider_element>());
					collision_controls.emplace_back(
						std::make_shared<ui::ok_cancel_control>(collision_dlg->_frame));

					if (collision_dlg->show_modal(collision_controls) != ui::close_result::ok) return;
					replace_existing = policy == collision_policy::replace;
				}

				detach_file_handles detach(s);
				shell_file_operation_ui processing(*view, parent);
				// A move empties the folders it came from, and only the destination was ever reported.
				// A search that names no folder is not watched, so it would keep listing what moved.
				df::unique_folders sources;
				df::item_element_ptr next;

				if (is_move)
				{
					for (const auto& path : items.file_paths(true)) sources.emplace(path.folder());
					for (const auto& path : items.folder_paths()) sources.emplace(path.parent());

					// #250: what the cursor should settle on once the set has gone, taken before the
					// operation because afterwards the items are no longer in the listing to step from.
					next = s.next_unselected_item();
				}

				const auto result = platform::move_or_copy(
					items.file_paths(true), items.folder_paths(), write_folder, is_move, replace_existing);

				if (result.success())
				{
					s.recent_folders.add(write_folder.text());
					s.item_index.queue_scan_folder(write_folder);
					if (!sources.empty()) s.item_index.queue_validate_changed_folders(std::move(sources));

					if (setting.show_results)
					{
						detach.keep_display_closed();
						s.open(view, df::search_t().add_selector(write_folder),
						       make_unique_paths(result.created_files));
					}
					else if (is_move)
					{
						// #249: the listing used to be updated only when the queued re-enumeration
						// happened to notice the folder had changed, which is why moved files
						// sometimes stayed. The move has completed here, so its own completion
						// re-runs the listing; the validation above still follows and confirms it.
						// #250: and the cursor lands on what followed the set rather than resetting,
						// so sorting a folder is not a re-navigation after every action.
						df::unique_paths selection;
						if (next) next->add_to(selection);
						s.open(view, s.search(), selection);
					}
				}
				else
				{
					// A run that stopped part way has still changed what it reached: the files it put
					// in the destination and, for a move, the folders it took them from. The index
					// learns of both either way, or those files stay out of every listing until
					// something else happens to rescan there.
					s.item_index.queue_scan_folder(write_folder);
					if (!sources.empty()) s.item_index.queue_validate_changed_folders(std::move(sources));

					if (result.code != platform::file_op_result_code::CANCELLED)
					{
						dlg->show_message(icon_index::error, title, result.format_error());
					}
				}
			}
		}
	}
}

static void repeat_mode_toggle(const view_state& s, const ui::control_frame_ptr& parent)
{
	auto m = setting.repeat;

	if (m == repeat_mode::repeat_all)
	{
		m = repeat_mode::repeat_one;
	}
	else if (m == repeat_mode::repeat_one)
	{
		m = repeat_mode::repeat_none;
	}
	else
	{
		m = repeat_mode::repeat_all;
	}

	setting.repeat = m;
	s.invalidate_view(view_invalid::tooltip | view_invalid::view_redraw | view_invalid::command_state);
}

static void font_invoke(const view_state& s, const ui::control_frame_ptr& parent)
{
	setting.large_font = !setting.large_font;
	s.invalidate_view(view_invalid::visual_options);
}

static void toggle_layout_scale_invoke(view_state& s, const ui::control_frame_ptr& parent)
{
	setting.step_item_scale(1);
}

static void toggle_details_invoke(const view_state& s, const bool only_toggle_selected)
{
	const auto sel = s.selected_item_group();

	if (only_toggle_selected)
	{
		if (sel.group)
		{
			sel.group->toggle_display();
		}
	}
	else
	{
		toggle_details_state = !toggle_details_state;
		const auto new_display = toggle_details_state
			                         ? df::item_group_display::detail
			                         : df::item_group_display::icons;

		for (const auto& g : s.groups())
		{
			g->display(new_display);

			// Persist the choice per media type so a subsequent search (e.g. clicking a
			// sidebar tag) recreates the groups with the same display instead of reverting. (#229)
			setting.set_detail_display(g->_key.type, new_display == df::item_group_display::detail);
		}

		s.invalidate_view(view_invalid::command_state);
	}
}

class open_with_auto_complete final : public ui::complete_strategy_t,
                                      public std::enable_shared_from_this<open_with_auto_complete>
{
	struct entry
	{
		std::string name;
		std::function<void(const std::vector<df::file_path>& files, const std::vector<df::folder_path>& folders)>
		invoke;
		int weight = 0;
	};

	class open_with_match final : public ui::auto_complete_match, public std::enable_shared_from_this<open_with_match>
	{
	public:
		entry _handler;
		ui::match_highlights _match;
		open_with_auto_complete& _parent;

		open_with_match(open_with_auto_complete& parent, entry h) : auto_complete_match(view_element_style::can_invoke),
		                                                            _handler(std::move(h)), _parent(parent)
		{
		}

		open_with_match(open_with_auto_complete& parent, entry h, ui::match_highlights match) :
			auto_complete_match(view_element_style::can_invoke), _handler(std::move(h)), _match(std::move(match)),
			_parent(parent)
		{
		}

		std::string edit_text() const override
		{
			return _handler.name;
		}

		void render(ui::draw_context& dc, const pointi element_offset) const override
		{
			const auto logical_bounds = bounds.offset(element_offset);
			const auto bg_color = calc_background_color(dc);

			if (bg_color.a > 0.0f)
			{
				const auto pad = padding * dc.scale_factor;
				dc.draw_rounded_rect(logical_bounds.inflate(pad.cx, pad.cy), bg_color, dc.padding1);
			}

			const auto highlight_clr = ui::color(ui::style::color::dialog_selected_text, dc.colors.alpha);
			const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);
			const auto highlights = make_highlights(_match, highlight_clr);

			dc.draw_text(_handler.name, highlights, logical_bounds, ui::style::font_face::dialog,
			             ui::style::text_style::single_line, clr, {});
		}

		void dispatch_event(const view_element_event& event) override
		{
			if (event.type == view_element_event_type::click)
			{
				_parent.selected(shared_from_this(), select_type::click);
			}
			else if (event.type == view_element_event_type::double_click)
			{
				_parent.selected(shared_from_this(), select_type::double_click);
			}
			else if (event.type == view_element_event_type::invoke)
			{
				const auto items = _parent._items;

				if (_handler.invoke)
				{
					_handler.invoke(items->file_paths(false), items->folder_paths());
				}

				_parent._state.recent_apps.add(_handler.name);
			}
		}

		view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
		                                             const pointi element_offset,
		                                             hit_test_context& ctx) override
		{
			return default_controller_from_location(*this, host, loc, element_offset, ctx);
		}
	};

	using open_with_match_ptr = std::shared_ptr<open_with_match>;

	df::hash_map<std::string, entry, df::ihash, df::ieq> _handlers;

	view_state& _state;
	ui::auto_complete_match_ptr _result;
	std::vector<command_info_ptr> _cmds;
	ui::frame_ptr _parent;
	std::shared_ptr<df::item_set> _items;

public:
	std::string no_results_message() override
	{
		return std::string(tt.type_to_search);
	}

	open_with_auto_complete(view_state& s, std::shared_ptr<df::item_set> items, ui::frame_ptr parent,
	                        std::vector<command_info_ptr> cmds) : _state(s), _cmds(std::move(cmds)),
	                                                              _parent(std::move(parent)), _items(std::move(items))
	{
		resize_to_show_results = false;
		max_predictions = 20u;
	}

	void initialise(const std::function<void(const ui::auto_complete_results&)> complete) override
	{
		for (const auto& c : _cmds)
		{
			if (!c->enable) continue;

			const auto name = c->text;
			df::assert_true(!str::is_empty(name));

			entry h;
			h.name = std::format("{} ({})", name, tt.open_with_tool);
			h.invoke = [c](const std::vector<df::file_path>& files, const std::vector<df::folder_path>& folders)
			{
				c->invoke();
			};
			h.weight = 1;
			_handlers[c->text] = h;
		}

		if (_items->single_file_extension())
		{
			const auto first_item = _items->items().front();
			const auto ext = first_item->extension();

			const auto platform_handlers = platform::assoc_handlers(ext);

			for (const auto& h : platform_handlers)
			{
				entry e;
				e.name = std::format("{} ({})", h.name, tt.open_with_app);
				e.invoke = h.invoke;
				e.weight = h.weight;
				_handlers[h.name] = e;
			}
		}

		df::string_counts counts;
		_state.recent_apps.count_strings(counts, 1);

		for (const auto& c : counts)
		{
			const auto found = _handlers.find(std::string(c.first));

			if (found != _handlers.cend())
			{
				found->second.weight += c.second;
			}
		}

		search({}, complete);
	}

	static bool compare_weight(const open_with_match_ptr& l, const open_with_match_ptr& r)
	{
		const auto diff = l->_handler.weight - r->_handler.weight;
		return diff == 0 ? str::icmp(l->_handler.name, r->_handler.name) < 0 : diff < 0;
	}

	void search(const std::string& query,
	            const std::function<void(const ui::auto_complete_results&)> complete) override
	{
		std::vector<open_with_match_ptr> results;

		if (str::is_empty(query))
		{
			for (const auto& i : _handlers)
			{
				results.emplace_back(std::make_shared<open_with_match>(*this, i.second));
			}
		}
		else
		{
			const auto query_parts = str::split(query, true);

			for (const auto& i : _handlers)
			{
				ui::match_highlights m;

				if (find_auto_complete(query_parts, i.first, false, m))
				{
					results.emplace_back(std::make_shared<open_with_match>(*this, i.second, m));
				}
			}
		}

		std::ranges::sort(results, compare_weight);
		df::assert_true(ui::is_ui_thread());
		complete(ui::auto_complete_results{results.begin(), results.end()});
	}

	void selected(const ui::auto_complete_match_ptr& i, const select_type st) override
	{
		_result = i;

		if (st == select_type::click || st == select_type::double_click)
		{
			_parent->close(false);
		}
	}

	ui::auto_complete_match_ptr selected() const override
	{
		return _result;
	}

	void invoke() const
	{
		if (_result)
		{
			const view_element_event e{view_element_event_type::invoke, nullptr};
			_result->dispatch_event(e);
		}
	}
};

static void open_with_invoke(view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view,
                             commands_map& commands)
{
	const auto title = tt.open_with_app_tool;
	const auto dlg = make_dlg(parent);

	pause_media pause(s);

	if (can_process_selection_or_explain(s, view, parent, title, df::process_items_type::local_file_or_folder))
	{
		std::vector<command_info_ptr> cmds =
		{
			commands[commands::tool_locate],
			commands[commands::tool_adjust_date],
			commands[commands::tool_burn],
			commands[commands::tool_convert],
			commands[commands::tool_desktop_background],
			commands[commands::tool_share],
			commands[commands::print],
			commands[commands::tool_rotate_anticlockwise],
			commands[commands::tool_rotate_clockwise],
			commands[commands::tool_edit_metadata],
		};

		const auto selected_items = std::make_shared<df::item_set>(s.selected_items());
		const auto complete = std::make_shared<open_with_auto_complete>(s, selected_items, dlg->_frame, cmds);

		std::string text;
		const std::vector<view_element_ptr> controls = {
			set_margin(std::make_shared<ui::title_control>(icon_index::next, title)),
			std::make_shared<ui::search_control>(dlg->_frame, text, complete),
			std::make_shared<divider_element>(),
			std::make_shared<ui::ok_cancel_control>(dlg->_frame)
		};


		if (ui::close_result::ok == dlg->show_modal(controls))
		{
			dlg->_frame->show(false);
			complete->invoke();
		}
	}
}

static std::string format_drive_details(const platform::drive_t& d)
{
	std::vector<std::string> parts;

	if (!d.vol_name.empty())
	{
		parts.emplace_back(std::format("{}: {}", tt_prep(tt.disk_label.sv()), d.vol_name));
	}

	// Capacity is zero for a card reader or optical drive with no media inserted.
	if (d.capacity.is_valid())
	{
		parts.emplace_back(std::format("{}: {}", tt_prep(tt.disk_capacity.sv()), d.capacity.str()));
		parts.emplace_back(std::format("{}: {}", tt_prep(tt.disk_used.sv()), d.used.str()));
	}

	if (!d.file_system.empty())
	{
		parts.emplace_back(std::format("{}: {}", tt_prep(tt.disk_system.sv()), d.file_system));
	}

	return str::combine(parts, ", ", false);
}

static void show_eject_dialog(view_state& s, const ui::control_frame_ptr& parent, const platform::drives& drives)
{
	const auto title = tt.command_eject;
	constexpr auto icon = icon_index::eject;
	auto dlg = make_dlg(parent);

	std::vector<view_element_ptr> controls = {
		set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon, title, tt.eject_help)),
		std::make_shared<divider_element>()
	};

	// Recorded by a drive button, reported once the dialog has closed. Showing a message from this
	// frame while it is inside show_modal would destroy the controls of the running button.
	const auto failed_drive = std::make_shared<std::string>();
	const auto ejecting = std::make_shared<bool>(false);
	auto found_removable = false;

	for (const auto& d : drives)
	{
		if (d.type != platform::drive_type::removable)
		{
			continue;
		}

		found_removable = true;

		controls.emplace_back(std::make_shared<ui::button_control>(
			dlg->_frame, drive_icon(d.type), str_format(tt.eject_title_fmt.sv(), d.name), format_drive_details(d),
			[&s, weak_frame = std::weak_ptr(dlg->_frame), path = df::folder_path(d.name), name = d.name, failed_drive,
				ejecting]
			{
				if (*ejecting) return;
				*ejecting = true;

				s.queue_async(async_queue::work, [&s, weak_frame, path, name, failed_drive]
				{
					const auto success = platform::eject(path);

					s.queue_ui([weak_frame, name, failed_drive, success]
					{
						const auto frame = weak_frame.lock();
						if (!frame) return;

						if (!success) *failed_drive = name;
						frame->close(!success);
					});
				});
			}));
	}

	if (!found_removable)
	{
		controls.emplace_back(set_margin(std::make_shared<text_element>(tt.eject_none_found)));
	}

	controls.emplace_back(std::make_shared<ui::button_control>(dlg->_frame, icon_index::close, tt.close,
	                                                           tt.eject_close_info, [f = dlg->_frame]
	                                                           {
		                                                           f->close(false);
	                                                           }));

	{
		pause_media pause(s);
		dlg->show_modal(controls);
	}

	if (!failed_drive->empty())
	{
		dlg.reset();
		make_dlg(parent)->show_message(icon_index::error, title,
		                               str_format(tt.eject_failed_fmt.sv(), *failed_drive));
	}
}

static void eject_invoke(view_state& s, const ui::control_frame_ptr& parent, std::function<void()> complete)
{
	s.queue_async(async_queue::work, [&s, weak_parent = std::weak_ptr(parent), complete = std::move(complete)]() mutable
	{
		auto drives = platform::scan_drives();

		s.queue_ui([&s, weak_parent, drives = std::move(drives), complete = std::move(complete)]
		{
			const auto parent_frame = weak_parent.lock();

			if (parent_frame)
			{
				show_eject_dialog(s, parent_frame, drives);
			}

			if (complete) complete();
		});
	});
}


static void scan_invoke(view_state& s, const ui::control_frame_ptr& parent, const view_host_base_ptr& view)
{
	const auto scan_result = platform::scan(s.save_path());

	if (scan_result.success)
	{
		record_feature_use(features::scan);

		df::unique_paths selection;
		selection.emplace(scan_result.saved_file_path);
		s.open(view, s.search(), selection);
	}
	else
	{
		const auto dlg = make_dlg(parent);
		dlg->show_message(icon_index::error, tt.command_scan,
		                  std::format("{}\n{}", tt.scan_failed, scan_result.error_message));
	}
};


static void browse_parent_invoke(view_state& s, const view_host_base_ptr& view)
{
	if (s.view_mode() != view_type::items)
	{
		s.view_mode(view_type::items);
	}
	else
	{
		const auto parent = s.parent_search();

		if (!parent.parent.is_empty())
		{
			s.open(view, parent.parent, make_unique_paths(parent.selection));
		}
	}
}

static void favorite_invoke(view_state& state, const ui::control_frame_ptr& parent)
{
	const auto search = state.search();
	const bool is_favorite = state.search_is_favorite();

	bool updated = false;

	if (is_favorite)
	{
		// Removal has to consider every slot. Sharing one loop with the add path meant an empty
		// slot before the favourite ended the scan on the Add dialog, so removing a favourite that
		// sat after a gap offered to add a second copy of it instead - and accepting that is how
		// the duplicate got there. Every copy goes, which also clears up any already made.
		for (auto i = 0; i < setting.search.count; i++)
		{
			if (str::is_empty(setting.search.path[i])) continue;

			if (search == df::search_t::parse(setting.search.path[i]))
			{
				setting.search.path[i].clear();
				setting.search.title[i].clear();
				updated = true;
			}
		}
	}
	else
	{
		for (auto i = 0; i < setting.search.count && !updated; i++)
		{
			if (!str::is_empty(setting.search.path[i])) continue;

			auto dlg = make_dlg(parent);
			auto dlg_parent = dlg->_frame;

			const auto text = str_format(tt.favorite_add_fmt.sv(), search.text());
			std::string title_text;

			if (search.selectors().size() == 1)
			{
				title_text = search.selectors().front().folder().name();
			}

			std::vector<view_element_ptr> controls = {
				set_margin(std::make_shared<ui::title_control>(icon_index::star_solid, tt.command_favorite)),
				std::make_shared<divider_element>(),
				set_margin(std::make_shared<text_element>(text)),
				set_margin(std::make_shared<text_element>(tt.favorite_title)),
				set_margin(std::make_shared<ui::edit_control>(dlg_parent, title_text)),
				std::make_shared<divider_element>(),
				std::make_shared<ui::ok_cancel_control>(dlg->_frame)
			};

			pause_media pause(state);

			if (dlg->show_modal(controls) == ui::close_result::ok)
			{
				setting.search.path[i] = search.text();
				setting.search.title[i] = title_text;
				updated = true;
			}
			else
			{
				// exit on cancel
				return;
			}
		}
	}

	if (updated)
	{
		state.invalidate_view(view_invalid::sidebar | view_invalid::command_state);
		state.update_search_is_favorite_or_collection_root();
	}
	else
	{
		auto dlg = make_dlg(parent);
		dlg->show_message(icon_index::folder, tt.command_favorite, tt.favorite_failed_to_add);
	}
}


#ifdef _DEBUG
void app_frame::run_test_action(const std::string_view action)
{
	if (action == "reset-graphics")
	{
		_state._events.free_graphics_resources(false, false);
		_app_frame->reset_graphics();
	}
	else if (action == "crash")
	{
		int* value = nullptr;
		*value = 19;
	}
	else if (action == "send-crash-report")
	{
		crash(known_path(platform::known_folder::test_files_folder).combine_file("Test.jpg"));
		_app_frame->show(true);
	}
	else if (action == "new-version")
	{
		setting.force_available_version = true;
		setting.min_show_update_day = 0;
		_state.invalidate_view(view_invalid::view_layout | view_invalid::app_layout);
	}
}
#endif


void app_frame::initialise_commands()
{
	update_command_text();

	_commands[commands::filter_photos]->text_can_change = true;
	_commands[commands::filter_videos]->text_can_change = true;
	_commands[commands::filter_audio]->text_can_change = true;
	_commands[commands::menu_group_toolbar]->text_can_change = true;
	// These toolbars are shared by several views, so their run buttons are renamed per view.
	// Without this the button keeps the text it was defined with and promises the wrong operation.
	_commands[commands::tool_run]->text_can_change = true;
	_commands[commands::edit_item_save_and_prev]->text_can_change = true;
	_commands[commands::edit_item_save_and_next]->text_can_change = true;

	_commands[commands::play]->icon_can_change = true;
	_commands[commands::view_fullscreen]->icon_can_change = true;
	_commands[commands::repeat_toggle]->icon_can_change = true;
	_commands[commands::playback_volume_toggle]->icon_can_change = true;
	_commands[commands::favorite]->icon_can_change = true;

	_commands[commands::option_highlight_large_items]->clr = app_colors::rank_background;
	_commands[commands::rate_rejected]->clr = color_rate_rejected;
	_commands[commands::label_approved]->clr = color_label_approved;
	_commands[commands::label_to_do]->clr = color_label_to_do;
	_commands[commands::label_select]->clr = color_label_select;
	_commands[commands::label_review]->clr = color_label_review;
	_commands[commands::label_second]->clr = color_label_second;

	const auto t = shared_from_this();

	add_command_invoke(commands::tool_adjust_date, [this]
	{
		_view_batch->mode(batch_tool_mode::adjust_date);
		update_toolbar_text(commands::tool_run, std::string(_view_batch->run_text().sv()));
		_state.view_mode(view_type::batch);
	});
	add_command_invoke(commands::tool_edit_metadata, [this]
	{
		_view_batch->mode(batch_tool_mode::metadata);
		reset_selector_selection_anchor();
		update_toolbar_text(commands::tool_run, std::string(_view_batch->run_text().sv()));
		_state.view_mode(view_type::batch);
	});
	add_command_invoke(commands::tool_edit_description, [this]
	{
		setting.set_artist = false;
		setting.set_caption = true;
		setting.set_album = false;
		setting.set_album_artist = false;
		setting.set_genre = false;
		setting.set_tv_show = false;
		setting.set_copyright_notice = false;
		setting.set_copyright_creator = false;
		setting.set_copyright_source = false;
		setting.set_copyright_credit = false;
		setting.set_copyright_url = false;
		setting.caption.clear();
		const auto display = _state.display_state();
		if (_state.selected_items().size() == 1 && display && display->is_one())
		{
			const auto md = display->_item1->metadata();
			if (md && !is_empty(md->description)) setting.caption = md->description.sv();
		}
		_view_batch->mode(batch_tool_mode::metadata);
		reset_selector_selection_anchor();
		update_toolbar_text(commands::tool_run, std::string(_view_batch->run_text().sv()));
		_state.view_mode(view_type::batch);
	});
	add_command_invoke(commands::exit, [this] { _app_frame->close(); });
	add_command_invoke(commands::playback_auto_play, [this] { setting.auto_play = !setting.auto_play; });
	add_command_invoke(commands::playback_auto_advance, [this] { setting.auto_advance = !setting.auto_advance; });
	add_command_invoke(commands::playback_last_played_pos,
	                   [this] { setting.last_played_pos = !setting.last_played_pos; });
	add_command_invoke(commands::playback_repeat_one, [this] { setting.repeat = repeat_mode::repeat_one; });
	add_command_invoke(commands::playback_repeat_none, [this] { setting.repeat = repeat_mode::repeat_none; });
	add_command_invoke(commands::playback_repeat_all, [this] { setting.repeat = repeat_mode::repeat_all; });
	add_command_invoke(commands::browse_back, [this] { _state.browse_back(_view_frame); });
	add_command_invoke(commands::browse_forward, [this] { _state.browse_forward(_view_frame); });

	add_command_invoke(commands::browse_next_folder, [this] { _state.open_next_path(_view_frame, true); });
	add_command_invoke(commands::browse_next_group, [this]
	{
		_state.select(_view_frame, _state.next_group_item(true), false, false, false);
	});
	add_command_invoke(commands::browse_next_item, [this] { _state.select_next(_view_frame, true, false, false); });
	add_command_invoke(commands::browse_next_item_extend,
	                   [this] { _state.select_next(_view_frame, true, true, false); });
	add_command_invoke(commands::browse_parent, [this] { browse_parent_invoke(_state, _view_frame); });
	add_command_invoke(commands::browse_previous_folder, [this] { _state.open_next_path(_view_frame, false); });
	add_command_invoke(commands::browse_previous_group, [this]
	{
		_state.select(_view_frame, _state.next_group_item(false), false, false, false);
	});
	add_command_invoke(commands::browse_previous_item,
	                   [this] { _state.select_next(_view_frame, false, false, false); });
	add_command_invoke(commands::browse_previous_item_extend,
	                   [this] { _state.select_next(_view_frame, false, true, false); });

	add_command_invoke(commands::tool_burn, [this] { burn_command_invoke(_state, _app_frame, _view_frame); });
	add_command_invoke(commands::tool_save_current_video_frame, [this] { capture_invoke(_state, _app_frame); });
	add_command_invoke(commands::favorite, [this] { favorite_invoke(_state, _app_frame); });
	add_command_invoke(commands::advanced_search, [this] { advanced_search_invoke(_state, _app_frame, _view_frame); });

	add_command_invoke(commands::view_minimize, [this] { _pa->sys_command(ui::sys_command_type::MINIMIZE); });
	add_command_invoke(commands::view_maximize, [this] { _pa->sys_command(ui::sys_command_type::MAXIMIZE); });
	add_command_invoke(commands::view_restore, [this] { _pa->sys_command(ui::sys_command_type::RESTORE); });
	// Either one is, to Windows, the window's maximize button. The platform decides when it offers snap layouts on it.
	find_or_create_command_info(commands::view_maximize)->caption_maximize = true;
	find_or_create_command_info(commands::view_restore)->caption_maximize = true;
	add_command_invoke(commands::view_close, [this] { _view->exit(); });
	add_command_invoke(commands::edit_item_save, [this]
	{
		if (_state.view_mode() == view_type::locate) _view_locate->run();
		else _view_edit->save_current();
	});
	add_command_invoke(commands::edit_item_save_and_prev, [this]
	{
		if (_state.view_mode() == view_type::locate) _view_locate->run_and_next(false);
		else _view_edit->save_and_next(false);
	});
	add_command_invoke(commands::edit_item_save_and_next, [this]
	{
		if (_state.view_mode() == view_type::locate) _view_locate->run_and_next(true);
		else _view_edit->save_and_next(true);
	});
	add_command_invoke(commands::edit_item_save_as, [this] { _view_edit->save_as(); });
	add_command_invoke(commands::edit_item_color_reset, [this] { _view_edit->color_reset(); });
	add_command_invoke(commands::tool_rotate_reset, [this] { _view_edit->rotate_reset(); });
	add_command_invoke(commands::edit_item_auto_color, [this] { _view_edit->auto_color(); });
	add_command_invoke(commands::edit_item_auto_document, [this] { _view_edit->auto_document(); });
	add_command_invoke(commands::edit_item_auto_straighten, [this] { _view_edit->auto_straighten(); });
	add_command_invoke(commands::edit_item_preview, [this] { _view_edit->toggle_preview(); });
	add_command_invoke(commands::tool_convert, [this]
	{
		_view_batch->mode(batch_tool_mode::convert);
		update_toolbar_text(commands::tool_run, std::string(_view_batch->run_text().sv()));
		_state.view_mode(view_type::batch);
	});
	add_command_invoke(commands::tool_copy_to_folder,
	                   [this] { copy_move_invoke(_state, _app_frame, _view_frame, false); });
	add_command_invoke(commands::tool_delete, [this] { delete_items(_state.selected_items()); });
	add_command_invoke(commands::tool_desktop_background, [this] { desktop_background_invoke(_state, _app_frame); });
	add_command_invoke(commands::tool_edit, [this] { edit_invoke(_state); });
	add_command_invoke(commands::tool_movie, [this] { _state.view_mode(view_type::movie); });
	add_command_invoke(commands::tool_movie_add, [this] { _view_movie->add_files(); });
	add_command_invoke(commands::tool_movie_remove, [this] { _view_movie->remove_current(); });
	add_command_invoke(commands::tool_movie_send_to_end, [this] { _view_movie->send_selection_to_end(); });
	add_command_invoke(commands::tool_movie_select_all, [this] { _view_movie->select_all(); });
	add_command_invoke(commands::tool_movie_open, [this] { _view_movie->open_project(); });
	add_command_invoke(commands::tool_movie_save, [this] { _view_movie->save_project(); });
	add_command_invoke(commands::tool_movie_import, [this] { _view_movie->import_project(); });
	add_command_invoke(commands::tool_movie_relink, [this] { _view_movie->relink_missing(); });
	add_command_invoke(commands::tool_movie_render, [this] { _view_movie->render_movie(); });
	add_command_invoke(commands::edit_copy, [this] { cut_copy_invoke(_state, _app_frame, _view_frame, false); });
	add_command_invoke(commands::edit_copy_item_path, [this] { copy_item_path_invoke(_state); });
	add_command_invoke(commands::edit_cut, [this] { cut_copy_invoke(_state, _app_frame, _view_frame, true); });
	add_command_invoke(commands::edit_paste, [this] { edit_paste_invoke(_state, _app_frame, _view_frame); });
	add_command_invoke(commands::tool_eject, [this]
	{
		eject_invoke(_state, _app_frame, [this] { invalidate_view(view_invalid::sidebar_drives); });
	});
	add_command_invoke(commands::tool_file_properties,
	                   [this] { file_properties_invoke(_state, _app_frame, _view_frame); });
	add_command_invoke(commands::browse_search, [this] { _search_edit->focus(); });
	add_command_invoke(commands::filter_items, [this] { _view_items->focus_rendered_filter(); });
	add_command_invoke(commands::select_all, [this] { _state.select_all(_view_frame); });
	add_command_invoke(commands::browse_recursive, [this] { show_flatten_invoke(_state, _app_frame, _view_frame); });
	add_command_invoke(commands::view_fullscreen, [this] { toggle_full_screen(); });
	add_command_invoke(commands::option_highlight_large_items, [this]
	{
		setting_invoke(_state, setting.highlight_large_items, !setting.highlight_large_items);
	});
	// A toggle, not a set: this is the one the menu shows, so clicking it while it is ticked has to
	// do something. Ascending stays an explicit set because the empty-results surface offers
	// whichever direction is not current by name.
	add_command_invoke(commands::sort_dates_descending, [this]
	{
		setting_invoke(_state, setting.sort_dates_descending, !setting.sort_dates_descending);
	});
	add_command_invoke(commands::sort_dates_ascending, [this]
	{
		setting_invoke(_state, setting.sort_dates_descending, false);
	});
	add_command_invoke(commands::options_collection, [this]
	{
		index_settings_invoke(_state, _app_frame, setting.collection);
	});
	add_command_invoke(commands::keyboard, [this] { show_keyboard_reference(_state, _app_frame, _commands); });
	add_command_invoke(commands::tool_locate, [this]
	{
		if (can_process_selection_or_explain(_state, _view_frame, _app_frame, tt.command_locate,
		                                     df::process_items_type::can_save_metadata))
		{
			_state.view_mode(view_type::locate);
		}
	});
	add_command_invoke(commands::tool_move_to_folder,
	                   [this] { copy_move_invoke(_state, _app_frame, _view_frame, true); });
	add_command_invoke(commands::view_show_sidebar, [this]
	{
		setting_invoke(_state, setting.show_sidebar, !setting.show_sidebar);
	});
	add_command_invoke(commands::tool_new_folder, [this] { new_folder_invoke(_state, _app_frame, _view_frame); });
#ifndef WINSTORE
	add_command_invoke(commands::info_new_version, [this] { show_update_dialog(_state, _app_frame); });
	add_command_invoke(commands::info_check_for_updates, [this] { show_update_dialog(_state, _app_frame); });
#endif
	add_command_invoke(commands::browse_open_containingfolder, [this]
	{
		containing_folder_invoke(_state, _app_frame, _view_frame);
	});
	add_command_invoke(commands::browse_open_googlemap, [this] { _state.open_gps_on_google_maps(); });
	add_command_invoke(commands::browse_open_in_file_browser, [this]
	{
		open_in_file_browser_invoke(_state, _app_frame, _view_frame);
	});
	add_command_invoke(commands::tool_open_with, [this]
	{
		open_with_invoke(_state, _app_frame, _view_frame, _commands);
	});
	add_command_invoke(commands::options_general, [this] { settings_invoke(_state, _app_frame); });
	add_command_invoke(commands::pin_item, [this] { pin_invoke(_state); });
	add_command_invoke(commands::play, [this] { _state.play(_view_frame); });
	add_command_invoke(commands::slideshow, [this] { _state.toggle_slideshow(_view_frame); });
	add_command_invoke(commands::print, [this] { print_invoke(_state, _app_frame, _view_frame); });
	add_command_invoke(commands::rate_none, [this] { rate_items_invoke(_state, _app_frame, _view_frame, 0); });
	add_command_invoke(commands::rate_1, [this] { rate_items_invoke(_state, _app_frame, _view_frame, 1); });
	add_command_invoke(commands::rate_2, [this] { rate_items_invoke(_state, _app_frame, _view_frame, 2); });
	add_command_invoke(commands::rate_3, [this] { rate_items_invoke(_state, _app_frame, _view_frame, 3); });
	add_command_invoke(commands::rate_4, [this] { rate_items_invoke(_state, _app_frame, _view_frame, 4); });
	add_command_invoke(commands::rate_5, [this] { rate_items_invoke(_state, _app_frame, _view_frame, 5); });
	add_command_invoke(commands::rate_rejected, [this]
	{
		const auto& items = _state.selected_items().items();
		const auto all_rejected = !items.empty() &&
			std::ranges::all_of(items, [](const df::item_element_ptr& item) { return item->rating() == -1; });
		rate_items_invoke(_state, _app_frame, _view_frame, all_rejected ? 0 : -1);
	});
	add_command_invoke(commands::label_select, [this]
	{
		label_items_invoke(_state, _app_frame, _view_frame, label_select_text);
	});
	add_command_invoke(commands::label_second, [this]
	{
		label_items_invoke(_state, _app_frame, _view_frame, label_second_text);
	});
	add_command_invoke(commands::label_approved, [this]
	{
		label_items_invoke(_state, _app_frame, _view_frame, label_approved_text);
	});
	add_command_invoke(commands::label_review, [this]
	{
		label_items_invoke(_state, _app_frame, _view_frame, label_review_text);
	});
	add_command_invoke(commands::label_to_do, [this]
	{
		label_items_invoke(_state, _app_frame, _view_frame, label_to_do_text);
	});
	add_command_invoke(commands::label_none, [this] { label_items_invoke(_state, _app_frame, _view_frame, {}); });
	add_command_invoke(commands::refresh, [this] { reload(); });
	add_command_invoke(commands::favorite_tags, [this] { favorite_tags_invoke(_state, _app_frame); });
	add_command_invoke(commands::search_related, [this] { related_invoke(_state, _app_frame, _view_frame); });
	add_command_invoke(commands::tool_rename, [this] { rename_invoke(_state, _app_frame, _view_frame); });
	add_command_invoke(commands::repeat_toggle, [this] { repeat_mode_toggle(_state, _app_frame); });
	add_command_invoke(commands::tool_rotate_anticlockwise, [this]
	{
		rotate_invoke(_state, _app_frame, _view_frame, simple_transform::rot_270);
	});
	add_command_invoke(commands::tool_rotate_clockwise, [this]
	{
		rotate_invoke(_state, _app_frame, _view_frame, simple_transform::rot_90);
	});
	add_command_invoke(commands::option_scale_up, [this]
	{
		setting_invoke(_state, setting.scale_up, !setting.scale_up);
	});
	add_command_invoke(commands::view_favorite_tags, [this]
	{
		setting.sidebar.show_favorite_tags_only = !setting.sidebar.show_favorite_tags_only;
		_state.invalidate_view(view_invalid::sidebar);
	});
	add_command_invoke(commands::tool_scan, [this] { scan_invoke(_state, _app_frame, _view_frame); });
	add_command_invoke(commands::options_sidebar, [this] { customise_invoke(_state, _app_frame); });
	add_command_invoke(commands::select_invert, [this] { _state.select_inverse(_view_frame); });
	add_command_invoke(commands::select_nothing, [this] { _state.select_nothing(_view_frame); });
	add_command_invoke(commands::tool_share, [this] { share_invoke(_state, _app_frame, _view_frame); });
	add_command_invoke(commands::option_show_rotated, [this]
	{
		setting_invoke(_state, setting.show_rotated, !setting.show_rotated);
	});
	add_command_invoke(commands::verbose_metadata, [this]
	{
		setting_invoke(_state, setting.verbose_metadata, !setting.verbose_metadata);
		invalidate_view(view_invalid::media_elements);
	});
	add_command_invoke(commands::show_raw_preview, [this]
	{
		setting_invoke(_state, setting.raw_preview, !setting.raw_preview);
		invalidate_view(view_invalid::media_elements);
	});
	add_command_invoke(commands::tool_tag, [this] { _state.view_mode(view_type::tags); });
	add_command_invoke(commands::option_toggle_details, [this]
	{
		toggle_details_invoke(_state, ui::current_key_state().shift);
	});
	add_command_invoke(commands::option_toggle_item_size, [this]
	{
		toggle_layout_scale_invoke(_state, _app_frame);
		invalidate_view(view_invalid::view_layout);
	});
	add_command_invoke(commands::view_help, [this] { about_invoke(_state, _app_frame, _commands); });
	add_command_invoke(commands::view_items, [this] { _view->exit(); });
	add_command_invoke(commands::large_font, [this] { font_invoke(_state, _app_frame); });
	add_command_invoke(commands::playback_volume_toggle, [this] { toggle_volume(); });
	add_command_invoke(commands::playback_volume200, [this] { setting.media_volume = media_volume_boost; });
	add_command_invoke(commands::playback_volume100, [this] { setting.media_volume = media_volumes[0]; });
	add_command_invoke(commands::playback_volume75, [this] { setting.media_volume = media_volumes[1]; });
	add_command_invoke(commands::playback_volume50, [this] { setting.media_volume = media_volumes[2]; });
	add_command_invoke(commands::playback_volume25, [this] { setting.media_volume = media_volumes[3]; });
	add_command_invoke(commands::playback_volume0, [this] { setting.media_volume = media_volumes[4]; });
	add_command_invoke(commands::view_zoom, [this] { zoom_invoke(_state, _app_frame); });
	add_command_invoke(commands::view_zoom_fit, [this] { zoom_fit_invoke(_state); });
	add_command_invoke(commands::view_zoom_fit_width,
	                   [this] { zoom_fit_variant_invoke(_state, df::zoom_scale_mode::fit_width); });
	add_command_invoke(commands::view_zoom_fill,
	                   [this] { zoom_fit_variant_invoke(_state, df::zoom_scale_mode::fill); });
	add_command_invoke(commands::view_zoom_toggle_fit, [this] { zoom_toggle_fit_invoke(_state); });
	add_command_invoke(commands::view_zoom_100, [this] { zoom_100_invoke(_state); });
	add_command_invoke(commands::view_zoom_in, [this] { zoom_step_invoke(_state, 1); });
	add_command_invoke(commands::view_zoom_pane_flip, [this]
	{
		if (const auto display = _state.display_state()) display->flip_zoom_pane();
	});
	add_command_invoke(commands::view_zoom_out, [this] { zoom_step_invoke(_state, -1); });
	add_command_invoke(commands::view_zoom_navigator_auto_hide,
	                   [this] { zoom_navigator_mode_invoke(_state, zoom_navigator_mode::auto_hide); });
	add_command_invoke(commands::view_zoom_navigator_off,
	                   [this] { zoom_navigator_mode_invoke(_state, zoom_navigator_mode::off); });

	add_command_invoke(commands::filter_photos, [this]
	{
		_state.filter().toggle(file_group::photo);
		invalidate_view(view_invalid::command_state | view_invalid::group_layout);
	});
	add_command_invoke(commands::filter_videos, [this]
	{
		_state.filter().toggle(file_group::video);
		invalidate_view(view_invalid::command_state | view_invalid::group_layout);
	});
	add_command_invoke(commands::filter_audio, [this]
	{
		_state.filter().toggle(file_group::audio);
		invalidate_view(view_invalid::command_state | view_invalid::group_layout);
	});

	add_command_invoke(commands::group_album, [this] { _state.group_order(group_by::album_show, {}); });
	add_command_invoke(commands::group_aspect_ratio, [this] { _state.group_order(group_by::aspect_ratio, {}); });
	add_command_invoke(commands::group_camera, [this] { _state.group_order(group_by::camera, {}); });
	add_command_invoke(commands::group_created, [this] { _state.group_order(group_by::date_created, {}); });
	add_command_invoke(commands::group_original, [this] { _state.group_order(group_by::date_original, {}); });
	add_command_invoke(commands::group_presence, [this] { _state.group_order(group_by::presence, {}); });
	add_command_invoke(commands::group_file_type, [this] { _state.group_order(group_by::file_type, {}); });
	add_command_invoke(commands::group_location, [this] { _state.group_order(group_by::location, {}); });
	add_command_invoke(commands::group_modified, [this] { _state.group_order(group_by::date_modified, {}); });
	add_command_invoke(commands::group_pixels, [this] { _state.group_order(group_by::resolution, {}); });
	add_command_invoke(commands::group_rating, [this] { _state.group_order(group_by::rating_label, {}); });
	add_command_invoke(commands::group_shuffle, [this] { _state.group_order(group_by::shuffle, {}); });
	add_command_invoke(commands::group_size, [this] { _state.group_order(group_by::size, {}); });
	add_command_invoke(commands::group_extension, [this] { _state.group_order(group_by::extension, {}); });
	add_command_invoke(commands::group_folder, [this] { _state.group_order(group_by::folder, {}); });
	add_command_invoke(commands::group_toggle, [this] { _state.toggle_group_order(); });

	add_command_invoke(commands::sort_def, [this] { _state.group_order({}, sort_by::def); });
	add_command_invoke(commands::sort_name, [this] { _state.group_order({}, sort_by::name); });
	add_command_invoke(commands::sort_size, [this] { _state.group_order({}, sort_by::size); });
	add_command_invoke(commands::sort_date_created, [this] { _state.group_order({}, sort_by::date_created); });
	add_command_invoke(commands::sort_date_original, [this] { _state.group_order({}, sort_by::date_original); });
	add_command_invoke(commands::sort_date_modified, [this] { _state.group_order({}, sort_by::date_modified); });

	add_command_invoke(commands::tool_import, [this] { _state.view_mode(view_type::import); });
	add_command_invoke(commands::tool_sync, [this] { _state.view_mode(view_type::sync); });

	add_command_invoke(commands::tool_run, [this]
	{
		if (_state.view_mode() == view_type::batch) _view_batch->run();
		else _view_rename->run();
	});

	add_command_invoke(commands::tool_refresh, [this]
	{
		if (_state.view_mode() == view_type::batch && _view_batch) _view_batch->refresh();
	});

	add_command_invoke(commands::import_analyze, [this] { _view_import->analyze(); });
	add_command_invoke(commands::import_run, [this] { _view_import->run(); });

	add_command_invoke(commands::locate_run, [this] { _view_locate->run(); });

	add_command_invoke(commands::sync_analyze, [this] { _view_sync->analyze(); });
	add_command_invoke(commands::sync_run, [this] { _view_sync->run(); });

	add_command_invoke(commands::tags_run, [this] { _view_tags->run(); });

	// Cancel stops the running task and leaves the view open; Close always exits the view.
	add_command_invoke(commands::view_cancel, [this] { if (_view) _view->cancel_operation(); });

	add_command_invoke(commands::english, [this]
	{
		setting.language = "en";
		tt.clear();
		language_changed("en");
	});

	_commands[commands::menu_main]->menu = [this]
	{
		std::vector<ui::command_ptr> result = {
			find_command(commands::view_fullscreen),
			find_command(commands::play),
			find_command(commands::slideshow),
			find_command(commands::browse_search),
			nullptr,
			find_command(commands::menu_navigate),
			find_command(commands::menu_open),
			find_command(commands::menu_tools),
			find_command(commands::menu_rate_or_label),
			find_command(commands::menu_select),
			find_command(commands::menu_group),
			find_command(commands::menu_options),
			nullptr,
			find_command(commands::tool_import),
			find_command(commands::tool_sync),
			find_command(commands::tool_scan),
			find_command(commands::refresh),
			find_command(commands::tool_new_folder),
			find_command(commands::favorite),
			nullptr,
			find_command(commands::edit_cut),
			find_command(commands::edit_copy),
			find_command(commands::edit_paste),
			nullptr,
			find_command(commands::keyboard),
			find_command(commands::view_help),
#ifndef WINSTORE
			find_command(commands::info_check_for_updates),
#endif
			find_command(commands::exit)
		};
		return result;
	};
	_commands[commands::menu_options]->menu = [this]
	{
		std::vector<ui::command_ptr> result = {
			find_command(commands::options_general),
			find_command(commands::options_collection),
			find_command(commands::options_sidebar),
			find_command(commands::favorite_tags),
			nullptr,
			find_command(commands::menu_display_options),
			find_command(commands::playback_menu),
			find_command(commands::menu_language)
		};
		return result;
	};
	_commands[commands::menu_open]->menu = [this]
	{
		std::vector<ui::command_ptr> result;
		auto selected_items = std::make_shared<df::item_set>(_state.selected_items());

		if (selected_items->single_file_extension())
		{
			const auto first_item = selected_items->items().front();
			const auto ext = first_item->extension();
			const auto handlers = platform::assoc_handlers(ext);

			for (const auto& h : handlers)
			{
				auto command = std::make_shared<ui::command>();
				command->text = str_format(tt.open_with_fmt.sv(), h.name);
				command->invoke = [h, selected_items, f = _app_frame]
				{
					const auto success = h.invoke(selected_items->file_paths(), selected_items->folder_paths());

					if (!success)
					{
						const auto dlg = make_dlg(f);
						dlg->show_message(icon_index::error, tt.open_with_title, tt.open_with_failed);
					}
				};

				result.emplace_back(command);
			}

			const auto tool_target = selected_external_tool_target(*selected_items);
			const auto file_tools = tool_target.is_empty()
				                        ? std::vector<std::shared_ptr<file_tool>>{}
				                        : first_item->file_type()->all_tools();

			if (!file_tools.empty())
			{
				std::vector<ui::command_ptr> file_tool_commands;

				for (const auto& t : file_tools)
				{
					if (t->exists())
					{
						auto command = std::make_shared<ui::command>();
						command->text = str_format(tt.open_with_fmt.sv(), t->text);
						command->invoke = [t, tool_target, f = _app_frame]
						{
							const auto success = t->invoke(tool_target);

							if (!success)
							{
								const auto dlg = make_dlg(f);
								dlg->show_message(icon_index::error, tt.open_with_title, tt.open_with_failed);
							}
						};

						file_tool_commands.emplace_back(command);
					}
				}

				if (!result.empty() && !file_tool_commands.empty())
				{
					result.emplace_back(nullptr);
				}

				result.insert(result.end(), file_tool_commands.begin(), file_tool_commands.end());
			}

			if (!result.empty())
			{
				result.emplace_back(nullptr);
			}
		}

		result.emplace_back(find_command(commands::tool_open_with));
		result.emplace_back(find_command(commands::browse_open_containingfolder));
		result.emplace_back(nullptr);

		if (_state.has_gps())
		{
			for (const auto& link : all_map_links())
			{
				auto command = std::make_shared<ui::command>();
				command->icon = icon_index::location;
				command->text = str_format(tt.open_with_fmt.sv(), link.text);
				command->invoke = [link, this] { _state.open_gps_on_map(link.url); };
				result.emplace_back(command);
			}
		}
		else
		{
			result.emplace_back(find_command(commands::browse_open_googlemap));
		}

		result.emplace_back(nullptr);
		result.emplace_back(find_command(commands::browse_open_in_file_browser));
		result.emplace_back(find_command(commands::tool_file_properties));
		return result;
	};
	_commands[commands::menu_language]->menu = [this]
	{
		const auto lang_folder = known_path(platform::known_folder::running_app_folder).combine("languages");
		const auto folder_contents = platform::iterate_file_items(lang_folder, false);

		std::vector<ui::command_ptr> result;
		result.emplace_back(find_command(commands::english));
		result.emplace_back(nullptr);

		for (const auto& f : folder_contents.files)
		{
			const auto lang_path = lang_folder.combine_file(f.name);
			const auto extension = lang_path.extension();

			if (str::icmp(extension, ".po") == 0)
			{
				const auto lang_code = lang_path.file_name_without_extension();

				auto command = std::make_shared<ui::command>();
				command->text = language_name(lang_code);
				command->checked = setting.language == lang_code;
				command->invoke = [this, lang_path, lang_code]
				{
					setting.language = lang_code;

					const auto po_entries = load_po(lang_path);
					tt.load_lang(lang_path.name(), po_entries);
					register_file_group_spellings();
					language_changed(lang_code);
				};

				result.emplace_back(command);
			}
		}
		return result;
	};
	// One definition of what a tool is. The panel affordance adds the file operations it also
	// carries, because the context menus already list those at their own top level.
	const auto tools_menu = [this]
	{
		std::vector<ui::command_ptr> result = {
			find_command(commands::tool_edit),
			find_command(commands::tool_movie),
			find_command(commands::tool_tag),
			find_command(commands::tool_locate),
			find_command(commands::tool_adjust_date),
			find_command(commands::tool_edit_metadata),
			nullptr,
			find_command(commands::tool_rotate_anticlockwise),
			find_command(commands::tool_rotate_clockwise),
			find_command(commands::tool_convert),
			find_command(commands::tool_save_current_video_frame),
			nullptr,
			find_command(commands::search_related),
			find_command(commands::tool_desktop_background),
			find_command(commands::tool_share),
			find_command(commands::tool_burn),
			find_command(commands::print),
		};
		return result;
	};
	_commands[commands::menu_tools]->menu = tools_menu;
	_commands[commands::menu_tools_toolbar]->menu = [this, tools_menu]
	{
		auto result = tools_menu();
		result.emplace_back(nullptr);
		result.emplace_back(find_command(commands::tool_delete));
		result.emplace_back(find_command(commands::tool_rename));
		result.emplace_back(find_command(commands::tool_copy_to_folder));
		result.emplace_back(find_command(commands::tool_move_to_folder));
		return result;
	};
	// The toolbar button and the menu entry open the same list; they only differ in their label.
	const auto group_menu = [this]
	{
		std::vector<ui::command_ptr> result = {
			find_command(commands::group_album),
			find_command(commands::group_aspect_ratio),
			find_command(commands::group_camera),
			find_command(commands::group_created),
			find_command(commands::group_original),
			find_command(commands::group_modified),
			find_command(commands::group_extension),
			find_command(commands::group_file_type),
			find_command(commands::group_location),
			find_command(commands::group_pixels),
			find_command(commands::group_presence),
			find_command(commands::group_rating),
			find_command(commands::group_size),
			find_command(commands::group_folder),
			nullptr,
			find_command(commands::sort_def),
			find_command(commands::sort_name),
			find_command(commands::sort_size),
			find_command(commands::sort_date_created),
			find_command(commands::sort_date_original),
			find_command(commands::sort_date_modified),
			nullptr,
			// A direction, not a sort key, so it sits outside the Sort by group. One item rather than the
			// ascending/descending pair it was: of those two the tick was permanently on one, and clicking
			// the ticked one set the value it already had.
			find_command(commands::sort_dates_descending),
			find_command(commands::group_shuffle),
			find_command(commands::group_toggle),
		};
		return result;
	};
	_commands[commands::menu_group]->menu = group_menu;
	_commands[commands::menu_group_toolbar]->menu = group_menu;
	_commands[commands::menu_select]->menu = [this]
	{
		std::vector<ui::command_ptr> result = {
			find_command(commands::browse_previous_item),
			find_command(commands::browse_previous_item_extend),
			find_command(commands::browse_next_item),
			find_command(commands::browse_next_item_extend),
			nullptr,
			find_command(commands::select_all),
			find_command(commands::select_invert),
			find_command(commands::select_nothing),
			nullptr,
			find_command(commands::pin_item)
		};
		return result;
	};

	_commands[commands::menu_rate_or_label]->menu = [this]
	{
		std::vector<ui::command_ptr> result = {
			find_command(commands::rate_1),
			find_command(commands::rate_2),
			find_command(commands::rate_3),
			find_command(commands::rate_4),
			find_command(commands::rate_5),
			nullptr,
			find_command(commands::rate_none),
			find_command(commands::rate_rejected),
			nullptr,
			find_command(commands::label_select),
			find_command(commands::label_second),
			find_command(commands::label_approved),
			find_command(commands::label_review),
			find_command(commands::label_to_do),
			find_command(commands::label_none)
		};

		return result;
	};
	_commands[commands::menu_navigate]->menu = [this]
	{
		std::vector<ui::command_ptr> result = {
			find_command(commands::browse_parent),
			find_command(commands::browse_back),
			find_command(commands::browse_forward),
			find_command(commands::browse_previous_folder),
			find_command(commands::browse_next_folder),
			find_command(commands::browse_previous_group),
			find_command(commands::browse_next_group),
			nullptr,
			find_command(commands::browse_recursive),
			nullptr,
			find_command(commands::advanced_search),
		};
		return result;
	};
	_commands[commands::menu_display_options]->menu = [this]
	{
		std::vector<ui::command_ptr> result = {
			find_command(commands::option_scale_up),
			find_command(commands::option_show_rotated),
			find_command(commands::option_highlight_large_items),
			find_command(commands::verbose_metadata),
			find_command(commands::show_raw_preview),
			nullptr,
			find_command(commands::large_font),
			nullptr,
			find_command(commands::view_show_sidebar),
			find_command(commands::option_toggle_item_size),
			find_command(commands::option_toggle_details)
		};

		return result;
	};
	_commands[commands::menu_zoom]->menu = [this]
	{
		std::vector<ui::command_ptr> result{
			find_command(commands::view_zoom),
			nullptr,
			find_command(commands::view_zoom_fit),
			find_command(commands::view_zoom_fit_width),
			find_command(commands::view_zoom_fill),
			find_command(commands::view_zoom_100),
			find_command(commands::view_zoom_in),
			find_command(commands::view_zoom_out),
			nullptr
		};
		const auto display = _state.display_state();
		const auto current_scale = display ? display->zoom_scale_percent() : 0;
		for (const auto scale : df::zoom_view_state::ladder())
		{
			auto command = std::make_shared<ui::command>();
			command->text = std::format("{}%", df::round(scale * 100.0));
			command->checked = current_scale == df::round(scale * 100.0);
			command->enable = display && display->can_zoom();
			command->invoke = [display, scale] { if (display) display->zoom_scale(scale); };
			result.emplace_back(std::move(command));
		}
		if (display && display->is_two())
		{
			result.emplace_back(nullptr);
			result.emplace_back(find_command(commands::view_zoom_pane_flip));
		}
		result.emplace_back(nullptr);
		result.emplace_back(find_command(commands::menu_zoom_navigator));
		return result;
	};
	_commands[commands::menu_zoom_navigator]->menu = [this]
	{
		std::vector<ui::command_ptr> result{
			find_command(commands::view_zoom_navigator_auto_hide),
			find_command(commands::view_zoom_navigator_off)
		};
		return result;
	};
	_commands[commands::menu_playback]->menu = _commands[commands::playback_menu]->menu = [this]
	{
		std::vector<ui::command_ptr> result = {
			find_command(commands::playback_volume200),
			find_command(commands::playback_volume100),
			find_command(commands::playback_volume75),
			find_command(commands::playback_volume50),
			find_command(commands::playback_volume25),
			find_command(commands::playback_volume0),
			nullptr,
			find_command(commands::playback_auto_play),
			find_command(commands::playback_auto_advance),
			find_command(commands::playback_last_played_pos),
			nullptr,
			find_command(commands::playback_repeat_all),
			find_command(commands::playback_repeat_one),
			find_command(commands::playback_repeat_none),
		};

		const auto devices = platform::list_audio_playback_devices();

		if (!devices.empty())
		{
			result.emplace_back(nullptr);
			auto heading = std::make_shared<ui::command>();
			heading->text = tt.audio_output_title;
			heading->enable = false;
			result.emplace_back(std::move(heading));

			const auto play_id = _player->play_audio_device_id();

			for (const auto& d : devices)
			{
				auto command = std::make_shared<ui::command>();
				command->text = d.name;
				command->checked = play_id == d.id;
				command->invoke = [this, d]
				{
					_state.change_audio_device(d.id);
					setting.sound_device = d.id;
					invalidate_view(view_invalid::options_save | view_invalid::command_state);
				};

				result.emplace_back(command);
			}
		}

		const auto& display = _state.display_state();

		if (display && display->is_one() && display->_player_media_info.has_multiple_audio_streams)
		{
			result.emplace_back(nullptr);
			auto heading = std::make_shared<ui::command>();
			heading->text = tt.audio_tracks_title;
			heading->enable = false;
			result.emplace_back(std::move(heading));

			auto audio_track_number = 0;
			for (const auto& st : display->_player_media_info.streams)
			{
				if (st.type == av_stream_type::audio)
				{
					++audio_track_number;

					auto command = std::make_shared<ui::command>();
					command->text = format_audio_stream_name(st, audio_track_number);
					command->checked = st.is_playing;
					command->invoke = [this, st]
					{
						_state.change_tracks(-1, st.index);
					};

					result.emplace_back(command);
				}
			}
		}

		return result;
	};

	for (const auto& c : _commands)
	{
		c.second->opaque = c.first;
	}
}
