// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Toolbars and how commands present. Builds the toolbars, decides each command's enabled,
// checked and visible state and its disabled reason, sets command text and icons, owns the default
// keyboard accelerators, and shows command tooltips. What a command does is registered in
// app_commands.cpp.

#include "pch.h"

#include "app_text.h"

#include "model_location.h"
#include "model_index.h"
#include "model_db.h"
#include "model.h"

#include "ui_dialog.h"
#include "view_items.h"
#include "view_sync.h"
#include "view_tags.h"
#include "view_import.h"
#include "view_rename.h"
#include "view_batch.h"
#include "view_locate.h"
#include "view_movie.h"
#include "view_edit.h"

#include "app_sidebar.h"
#include "app_commands.h"
#include "app.h"

#include <utility>

bool toggle_details_state = false;

// The address bar shows the scope of the current search: a folder, a folder searched recursively,
// or the kind of the first term. The star glyph is reserved for ratings, so fall back to the
// generic search icon rather than showing it here.
static icon_index address_icon(const df::search_t& search)
{
	if (search.has_recursive_selector()) return icon_index::recursive;
	if (search.has_selector()) return icon_index::folder;

	if (search.has_terms())
	{
		const auto icon = search.first_type()->icon;
		if (icon != icon_index::star && icon != icon_index::none) return icon;
	}

	return icon_index::search;
}

void app_frame::create_toolbars()
{
	const std::vector<ui::command_ptr> tbButtonsNav1 =
	{
		find_command(commands::view_show_sidebar),
#ifndef WINSTORE
		find_command(commands::info_new_version),
#endif
		find_command(commands::browse_back),
		find_command(commands::browse_forward),
		find_command(commands::browse_parent),
		find_command(commands::refresh),
	};

	const std::vector<ui::command_ptr> tbButtonsNav2 =
	{
		find_command(commands::favorite),
		find_command(commands::options_collection),
		find_command(commands::browse_previous_folder),
		find_command(commands::browse_next_folder),
		find_command(commands::advanced_search),
		find_command(commands::menu_main),
	};

	const std::vector<ui::command_ptr> tbButtonsNav3 =
	{
		find_command(commands::view_minimize),
		find_command(commands::view_maximize),
		find_command(commands::view_restore),
		find_command(commands::exit),
	};

	const std::vector<ui::command_ptr> media_edit_commands =
	{
		find_command(commands::edit_item_preview),
		find_command(commands::edit_item_save_and_prev),
		find_command(commands::edit_item_save_and_next),
		find_command(commands::edit_item_save_as),
		find_command(commands::edit_item_save),
		nullptr,
		find_command(commands::view_maximize),
		find_command(commands::view_restore),
		find_command(commands::view_close),
	};

	const std::vector<ui::command_ptr> sync_commands =
	{
		find_command(commands::sync_analyze),
		find_command(commands::sync_run),
		find_command(commands::view_cancel),
		nullptr,
		find_command(commands::view_maximize),
		find_command(commands::view_restore),
		find_command(commands::view_close),
	};

	const std::vector<ui::command_ptr> tool_commands =
	{
		find_command(commands::tool_refresh),
		find_command(commands::tool_run),
		find_command(commands::view_cancel),
		nullptr,
		find_command(commands::view_maximize),
		find_command(commands::view_restore),
		find_command(commands::view_close),
	};

	const std::vector<ui::command_ptr> import_commands =
	{
		find_command(commands::import_analyze),
		find_command(commands::import_run),
		find_command(commands::view_cancel),
		nullptr,
		find_command(commands::view_maximize),
		find_command(commands::view_restore),
		find_command(commands::view_close),
	};

	const std::vector<ui::command_ptr> tags_commands =
	{
		find_command(commands::tags_run),
		find_command(commands::view_cancel),
		nullptr,
		find_command(commands::view_maximize),
		find_command(commands::view_restore),
		find_command(commands::view_close),
	};

	const std::vector<ui::command_ptr> busy_commands =
	{
		find_command(commands::view_cancel),
	};

	const std::vector<ui::command_ptr> locate_commands =
	{
		find_command(commands::edit_item_save_and_prev),
		find_command(commands::edit_item_save_and_next),
		find_command(commands::locate_run),
		nullptr,
		find_command(commands::view_maximize),
		find_command(commands::view_restore),
		find_command(commands::view_close),
	};

	const std::vector<ui::command_ptr> movie_commands =
	{
		find_command(commands::tool_movie_render),
		find_command(commands::tool_movie_add),
		find_command(commands::tool_movie_remove),
		find_command(commands::tool_movie_relink),
		find_command(commands::tool_movie_open),
		find_command(commands::tool_movie_save),
		find_command(commands::tool_movie_import),
		find_command(commands::view_cancel),
		nullptr,
		find_command(commands::view_maximize),
		find_command(commands::view_restore),
		find_command(commands::view_close),
	};

	ui::toolbar_styles tb_styles;
	tb_styles.button_extent = {30, 40};
	_navigate1 = _app_frame->create_toolbar(tb_styles, tbButtonsNav1);
	_navigate2 = _app_frame->create_toolbar(tb_styles, tbButtonsNav2);

	tb_styles.button_extent = {40, 40};
	_navigate3 = _app_frame->create_toolbar(tb_styles, tbButtonsNav3);

	ui::edit_styles search_styles;
	search_styles.horizontal_scroll = true;
	search_styles.rounded_corners = true;
	search_styles.select_all_on_focus = true;
	search_styles.bg_clr = ui::style::color::toolbar_background;
	search_styles.capture_key_down = [this](const int key_code, const ui::key_state keys)
	{
		const auto key = static_cast<char32_t>(key_code);

		if (key == keys::RETURN)
		{
			search_enter();
			return true;
		}
		if (key == keys::ESCAPE)
		{
			cancel_search_edit();
			return true;
		}
		if (key == keys::UP || key == keys::DOWN)
		{
			if (_search_predictions_frame)
			{
				_search_predictions_frame->step_selection(key == keys::UP ? -1 : 1);
			}
			return true;
		}
		return key == keys::TAB && !keys.control && !keys.shift && search_accept_selected();
	};
	_search_edit = _app_frame->create_edit(search_styles, {}, [this](const std::string_view text)
	{
		search_text_changed(text);
	});

	tb_styles.xTBSTYLE_LIST = true;
	tb_styles.button_extent = {0, 0};

	_media_edit_commands = _app_frame->create_toolbar(tb_styles, media_edit_commands);
	_tool_commands = _app_frame->create_toolbar(tb_styles, tool_commands);
	_import_commands = _app_frame->create_toolbar(tb_styles, import_commands);
	_locate_commands = _app_frame->create_toolbar(tb_styles, locate_commands);
	_sync_commands = _app_frame->create_toolbar(tb_styles, sync_commands);
	_tags_commands = _app_frame->create_toolbar(tb_styles, tags_commands);
	_movie_commands = _app_frame->create_toolbar(tb_styles, movie_commands);
	_busy_commands = _app_frame->create_toolbar(tb_styles, busy_commands);
}

// locations.md 7.1, baseline defect 5: this button now reads only what the grouping and sorting
// are, because that is the menu it opens. The totals moved to their own affordance next to it,
// which has no menu, so a user reaching for the count is no longer offered a grouping menu.
std::string format_items_summary(const group_by grouping, const sort_by order,
                                 const df::file_group_histogram& summary, const bool is_init_complete)
{
	std::string_view group_text;
	std::string_view sort_text;

	switch (grouping)
	{
	case group_by::presence: group_text = tt.sort_by_presence;
		break;
	case group_by::file_type: group_text = tt.sort_by_file_type;
		break;
	case group_by::shuffle: group_text = tt.sort_by_shuffle;
		break;
	case group_by::size: group_text = tt.sort_by_size;
		break;
	case group_by::extension: group_text = tt.sort_by_extension;
		break;
	case group_by::location: group_text = tt.sort_by_location;
		break;
	case group_by::rating_label: group_text = tt.sort_by_rating_label;
		break;
	case group_by::date_created: group_text = tt.prop_name_created;
		break;
	case group_by::date_original: group_text = tt.prop_name_original;
		break;
	case group_by::date_modified: group_text = tt.prop_name_modified;
		break;
	case group_by::camera: group_text = tt.prop_name_camera;
		break;
	case group_by::album_show: group_text = tt.sort_by_album_show;
		break;
	case group_by::resolution: group_text = tt.sort_by_resolution;
		break;
	case group_by::aspect_ratio: group_text = tt.sort_by_aspect_ratio;
		break;
	case group_by::folder: group_text = tt.sort_by_Folder;
		break;
	case group_by::related: group_text = tt.command_related;
		break;
	}

	switch (order)
	{
	case sort_by::def: sort_text = tt.sort_by_def;
		break;
	case sort_by::name: sort_text = tt.sort_by_name;
		break;
	case sort_by::size: sort_text = tt.sort_by_size;
		break;
	case sort_by::date_created: sort_text = tt.prop_name_created;
		break;
	case sort_by::date_original: sort_text = tt.prop_name_original;
		break;
	case sort_by::date_modified: sort_text = tt.prop_name_modified;
		break;
	}

	const auto total_items = summary.total_items();
	const auto has_content = total_items.count > 0 || summary.total_folders().count > 0;

	if (!has_content)
	{
		return std::string(is_init_complete ? tt.empty : tt.loading);
	}

	if (group_text == sort_text || grouping == group_by::shuffle || sort_text.empty() || order == sort_by::def)
	{
		return str_format(tt.grouped_by_fmt.sv(), group_text);
	}

	return str_format(tt.grouped_and_sorted_fmt.sv(), group_text, sort_text);
}

// locations.md 7.1: the totals read as totals and nothing else. Hovering shows the breakdown by
// type; there is no menu behind them.
std::string format_items_totals(const df::file_group_histogram& summary, const bool is_init_complete)
{
	const auto total_items = summary.total_items();

	if (total_items.count > 0)
	{
		return std::format("{}|{}", str::format_count(total_items.count), df::format_size(total_items.size));
	}

	const auto total_folders = summary.total_folders();

	if (total_folders.count > 0)
	{
		return std::format("{} {}", str::format_count(total_folders.count), tt.folders);
	}

	return std::string(is_init_complete ? tt.empty : tt.loading);
}

icon_index volumes_icons[5] = {
	icon_index::volume3, icon_index::volume2, icon_index::volume1, icon_index::volume0, icon_index::mute
};

void app_frame::toggle_volume()
{
	auto v = setting.media_volume;

	if (v == 0)
	{
		v = media_volumes[0];
	}
	else
	{
		for (const int volume : media_volumes)
		{
			if (v > volume)
			{
				v = volume;
				break;
			}
		}
	}

	setting.media_volume = std::clamp(v, 0, media_volume_boost);
	_commands[commands::playback_volume_toggle]->icon = sound_icon();
	invalidate_view(view_invalid::command_state);
}

icon_index app_frame::sound_icon()
{
	const auto v = setting.media_volume;

	for (auto i = 0u; i < std::size(media_volumes); i++)
	{
		if (v >= media_volumes[i])
		{
			return volumes_icons[i];
		}
	}

	return icon_index::mute;
}


icon_index app_frame::repeat_toggle_icon()
{
	if (setting.repeat == repeat_mode::repeat_one) return icon_index::repeat_one;
	if (setting.repeat == repeat_mode::repeat_all) return icon_index::repeat_all;
	return icon_index::repeat_none;
}

bool app_frame::update_toolbar_text(const commands cc, const std::string& text)
{
	const auto command = _commands[cc];
	const auto changed = text != command->toolbar_text;

	if (changed)
	{
		command->toolbar_text = text;
		// The items view measures its rendered toolbars from this text, so the view has to
		// re-lay out as well as the frame.
		invalidate_view(view_invalid::app_layout | view_invalid::view_layout);
	}

	return changed;
}

// One place that names which run command each task view answers to. While a command dialog is up
// it owns the keyboard, so the task view claims nothing.
commands app_frame::task_view_run_command() const
{
	if (df::command_active != 0) return commands::none;

	switch (_state.view_mode())
	{
	case view_type::rename:
	case view_type::batch: return commands::tool_run;
	case view_type::import: return commands::import_run;
	case view_type::sync: return commands::sync_run;
	case view_type::locate: return commands::locate_run;
	case view_type::tags: return commands::tags_run;
	default: return commands::none;
	}
}

void app_frame::update_button_state(const bool resize)
{
	// Not cached: the update snooze is measured in days, so a session left open overnight has to
	// see the new day arrive.
	const auto now_days = platform::now().to_days();
	if (!_has_burner) _has_burner = platform::has_burner();
	const auto has_burner = *_has_burner;

	const auto view_mode = _state.view_mode();
	const auto view_processing = _view && _view->progress().active;
	const auto selection_status = _state.selection_status();
	const auto is_playing = selection_status.is_playing;
	const auto is_playing_media = selection_status.is_playing_media;
	const auto can_play_media = selection_status.can_play_media;
	const auto is_slideshow = selection_status.is_slideshow;
	const auto can_zoom = selection_status.can_zoom;
	const auto display = _state.display_state();
	const auto is_maximized = _app_frame->is_maximized();
	const auto is_edit_view = df::command_active == 0 && view_mode == view_type::edit;
	const auto is_items_view = df::command_active == 0 && view_mode == view_type::items;
	const auto is_media_view = df::command_active == 0 && view_mode == view_type::media;
	const auto is_media_or_items_view = df::command_active == 0 && (view_mode == view_type::items || view_mode ==
		view_type::media);
	const auto has_selection = is_media_or_items_view && _state.has_selection();
	const auto has_movie_selection = is_media_or_items_view && std::any_of(
		_state.selected_items().items().begin(), _state.selected_items().items().end(), [](const auto& item)
		{
			const auto* const type = item->file_type();
			return type && (type->group == file_group::photo || type->group == file_group::video);
		});
	const auto is_single_media_selection = is_media_or_items_view && selection_status.has_single_media_selection;
	const auto local_files_result = _state.selection_process_result(df::process_items_type::local_file);
	const auto local_items_result = _state.selection_process_result(df::process_items_type::local_file_or_folder);
	const auto save_metadata_result = _state.selection_process_result(df::process_items_type::can_save_metadata);
	const auto save_pixels_result = _state.selection_process_result(df::process_items_type::can_save_pixels);
	const auto photos_only_result = _state.selection_process_result(df::process_items_type::photos_only);
	const auto can_process_local_files = is_media_or_items_view && local_files_result.success();
	const auto can_process_local_items = is_media_or_items_view && local_items_result.success();
	const auto can_save_metadata = is_media_or_items_view && save_metadata_result.success();
	const auto can_save_pixels = is_media_or_items_view && save_pixels_result.success();
#ifndef WINSTORE
	const auto new_version_avail = is_media_or_items_view &&
		df::version(s_app_version) < df::version(setting.available_version) && static_cast<int>(now_days) >= setting.
		min_show_update_day;
	const auto show_new_version = is_media_or_items_view && (setting.force_available_version || new_version_avail);
#endif
	const auto command_item = _state.command_item();
	const auto is_displaying_item = is_media_or_items_view && command_item;
	const auto search_has_selector = _state.search().has_selector(); // This line remains unchanged
	const auto has_save_folder = _state.search().is_showing_folder();

#ifndef WINSTORE
	_commands[commands::info_new_version]->visible = !is_edit_view && show_new_version;
	// The whole point of this button is to be noticed, so it carries the accent fill.
	_commands[commands::info_new_version]->highlight = true;
#endif
	_commands[commands::view_maximize]->visible = !is_maximized;
	_commands[commands::view_restore]->visible = is_maximized;
	_commands[commands::view_show_sidebar]->visible = !is_edit_view;
	_commands[commands::browse_forward]->visible = !is_edit_view;

	_commands[commands::advanced_search]->enable = is_media_or_items_view;
	_commands[commands::browse_back]->enable = is_media_or_items_view && _state.history.can_browse_back();
	_commands[commands::browse_forward]->enable = is_media_or_items_view && _state.history.can_browse_forward();
	_commands[commands::browse_next_folder]->enable = is_items_view && _state.has_next_path(true);
	_commands[commands::browse_next_group]->enable = is_media_or_items_view && _state.has_display_items();
	_commands[commands::browse_next_item]->enable = is_media_or_items_view && _state.has_display_items();
	_commands[commands::browse_next_item_extend]->enable = is_media_or_items_view && _state.has_display_items();
	_commands[commands::browse_open_containingfolder]->enable = is_media_or_items_view && is_displaying_item;
	_commands[commands::browse_open_googlemap]->enable = is_media_or_items_view && _state.has_gps();
	_commands[commands::browse_open_in_file_browser]->enable = is_media_or_items_view && has_selection;
	// In the media view Parent returns to the items view; in the items view it broadens the scope.
	_commands[commands::browse_parent]->enable = is_media_view || (is_items_view && _state.has_parent_search());
	_commands[commands::browse_previous_folder]->enable = is_items_view && _state.has_next_path(false);
	_commands[commands::browse_previous_group]->enable = is_media_or_items_view && _state.has_display_items();
	_commands[commands::browse_previous_item]->enable = is_media_or_items_view && _state.has_display_items();
	_commands[commands::browse_previous_item_extend]->enable = is_media_or_items_view && _state.has_display_items();
	_commands[commands::browse_recursive]->enable = is_items_view && search_has_selector;
	_commands[commands::browse_search]->enable = is_items_view;
	_commands[commands::edit_copy]->enable = can_process_local_items;
	_commands[commands::edit_copy_item_path]->enable = can_process_local_items;
	// Cut runs the same eligibility test as Copy when invoked, so it answers the same way here. Enabled
	// on a bare selection, it offered itself for offline items and outside the item views only to
	// refuse once chosen.
	_commands[commands::edit_cut]->enable = can_process_local_items;
	const auto can_edit_pixels = is_edit_view && _view_edit && _view_edit->can_edit_pixels();
	_commands[commands::edit_item_auto_color]->enable = can_edit_pixels;
	_commands[commands::edit_item_auto_document]->enable = can_edit_pixels;
	_commands[commands::edit_item_auto_straighten]->enable = can_edit_pixels;
	_commands[commands::edit_item_preview]->enable = can_edit_pixels;
	_commands[commands::edit_item_color_reset]->enable = can_edit_pixels;
	_commands[commands::edit_item_save]->enable = is_edit_view && _state._edit_item && edit_has_changes();
	// In the locate view these run the same operation as locate_run, so they answer to the same
	// test: an enabled button that writes nothing reads as a broken command.
	const auto can_locate = view_mode == view_type::locate && _view_locate && _view_locate->can_run();
	_commands[commands::edit_item_save_and_next]->enable = is_edit_view || can_locate;
	_commands[commands::edit_item_save_and_prev]->enable = is_edit_view || can_locate;
	const auto locate_view = view_mode == view_type::locate;
	const auto previous_text = locate_view ? tt.command_locate_and_back.sv() : tt.command_save_and_back.sv();
	const auto next_text = locate_view ? tt.command_locate_and_next.sv() : tt.command_save_and_next.sv();
	update_toolbar_text(commands::edit_item_save_and_prev, std::string(previous_text));
	update_toolbar_text(commands::edit_item_save_and_next, std::string(next_text));
	_commands[commands::edit_item_save_and_prev]->text = previous_text;
	_commands[commands::edit_item_save_and_next]->text = next_text;
	_commands[commands::edit_item_save_and_prev]->tooltip_text = locate_view
		                                                             ? std::string{}
		                                                             : std::string(
			                                                             tt.command_save_and_back_tooltip.sv());
	_commands[commands::edit_item_save_and_next]->tooltip_text = locate_view
		                                                             ? std::string{}
		                                                             : std::string(
			                                                             tt.command_save_and_next_tooltip.sv());
	_commands[commands::edit_item_save_as]->enable = can_edit_pixels;
	_commands[commands::edit_paste]->enable = is_items_view && has_save_folder;
	_commands[commands::english]->enable = true;
	_commands[commands::exit]->enable = true;
	_commands[commands::favorite]->enable = is_media_or_items_view;
	_commands[commands::filter_items]->enable = is_items_view;
	_commands[commands::filter_audio]->enable = is_items_view;
	_commands[commands::filter_photos]->enable = is_items_view;
	_commands[commands::filter_videos]->enable = is_items_view;
	// A related search groups by relation, so choosing a grouping would change nothing visible.
	const auto can_group = is_items_view && _state.effective_group_order() == _state.group_order();

	_commands[commands::group_album]->enable = can_group;
	_commands[commands::group_aspect_ratio]->enable = can_group;
	_commands[commands::group_camera]->enable = can_group;
	_commands[commands::group_created]->enable = can_group;
	_commands[commands::group_original]->enable = can_group;
	_commands[commands::group_extension]->enable = can_group;
	_commands[commands::group_file_type]->enable = can_group;
	_commands[commands::group_folder]->enable = can_group;
	_commands[commands::group_location]->enable = can_group;
	_commands[commands::group_modified]->enable = can_group;
	_commands[commands::group_pixels]->enable = can_group;
	_commands[commands::group_presence]->enable = can_group;
	_commands[commands::group_rating]->enable = can_group;
	_commands[commands::group_shuffle]->enable = can_group;
	_commands[commands::group_size]->enable = can_group;
	_commands[commands::group_toggle]->enable = can_group;
	_commands[commands::import_analyze]->enable = view_mode == view_type::import && !view_processing && _view_import &&
		_view_import->can_analyze();
	_commands[commands::import_run]->enable = view_mode == view_type::import && !view_processing && _view_import &&
		_view_import->can_run();
#ifndef WINSTORE
	// Must match the visibility test above: a button that appears only to sit dimmed reads as broken.
	_commands[commands::info_new_version]->enable = is_media_or_items_view;
	_commands[commands::info_check_for_updates]->enable = true;
#endif
	// The keyboard reference reads command state; like Help it answers from anywhere.
	_commands[commands::keyboard]->enable = true;
	_commands[commands::label_approved]->enable = can_save_metadata;
	_commands[commands::label_none]->enable = can_save_metadata;
	_commands[commands::label_review]->enable = can_save_metadata;
	_commands[commands::label_second]->enable = can_save_metadata;
	_commands[commands::label_select]->enable = can_save_metadata;
	_commands[commands::label_to_do]->enable = can_save_metadata;
	_commands[commands::large_font]->enable = true;
	_commands[commands::menu_display_options]->enable = true;
	_commands[commands::menu_group]->enable = true;
	_commands[commands::menu_group_toolbar]->enable = true;
	_commands[commands::menu_language]->enable = true;
	_commands[commands::menu_main]->enable = true;
	_commands[commands::menu_navigate]->enable = true;
	_commands[commands::menu_open]->enable = has_selection;
	_commands[commands::menu_options]->enable = true;
	_commands[commands::menu_playback]->enable = is_media_or_items_view;
	auto playback_toolbar_text = std::string(tt.command_playback_toolbar.sv());

	if (display && display->is_one() && display->_player_media_info.has_multiple_audio_streams)
	{
		auto audio_track_number = 0;

		for (const auto& stream : display->_player_media_info.streams)
		{
			if (stream.type != av_stream_type::audio) continue;
			++audio_track_number;

			if (stream.is_playing)
			{
				playback_toolbar_text = str_format(tt.audio_track_current_fmt.sv(),
				                                   format_audio_stream_name(stream, audio_track_number));
				break;
			}
		}
	}

	update_toolbar_text(commands::menu_playback, playback_toolbar_text);
	// The parent must answer the same test as the entries it opens, or it promises what they refuse.
	_commands[commands::menu_rate_or_label]->enable = can_save_metadata;
	_commands[commands::menu_select]->enable = true;
	_commands[commands::menu_tools]->enable = has_selection;
	_commands[commands::menu_tools_toolbar]->enable = has_selection;
	_commands[commands::option_highlight_large_items]->enable = is_items_view;
	_commands[commands::option_scale_up]->enable = is_media_or_items_view;
	_commands[commands::option_show_rotated]->enable = is_media_or_items_view;
	_commands[commands::option_toggle_details]->enable = is_items_view;
	_commands[commands::option_toggle_item_size]->enable = is_items_view;
	_commands[commands::options_collection]->enable = is_media_or_items_view;
	_commands[commands::options_general]->enable = is_media_or_items_view;
	_commands[commands::options_sidebar]->enable = is_media_or_items_view;
	_commands[commands::favorite_tags]->enable = is_media_or_items_view || view_mode == view_type::tags;
	_commands[commands::pin_item]->enable = is_media_or_items_view && (_state.has_pin() || _state.focus_item() !=
		nullptr);
	_commands[commands::play]->enable = is_media_or_items_view && (can_play_media || is_playing);
	_commands[commands::slideshow]->enable = is_media_or_items_view && _state.can_slideshow();
	_commands[commands::playback_auto_play]->enable = is_media_or_items_view;
	_commands[commands::playback_auto_advance]->enable = is_media_or_items_view;
	_commands[commands::playback_last_played_pos]->enable = is_media_or_items_view;
	_commands[commands::playback_menu]->enable = is_media_or_items_view;
	_commands[commands::playback_repeat_all]->enable = is_media_or_items_view;
	_commands[commands::playback_repeat_none]->enable = is_media_or_items_view;
	_commands[commands::playback_repeat_one]->enable = is_media_or_items_view;
	_commands[commands::playback_volume_toggle]->enable = is_media_or_items_view;
	_commands[commands::playback_volume0]->enable = is_media_or_items_view;
	_commands[commands::playback_volume100]->enable = is_media_or_items_view;
	_commands[commands::playback_volume200]->enable = is_media_or_items_view;
	_commands[commands::playback_volume25]->enable = is_media_or_items_view;
	_commands[commands::playback_volume50]->enable = is_media_or_items_view;
	_commands[commands::playback_volume75]->enable = is_media_or_items_view;
	_commands[commands::print]->enable = can_process_local_files;
	_commands[commands::rate_1]->enable = can_save_metadata;
	_commands[commands::rate_2]->enable = can_save_metadata;
	_commands[commands::rate_3]->enable = can_save_metadata;
	_commands[commands::rate_4]->enable = can_save_metadata;
	_commands[commands::rate_5]->enable = can_save_metadata;
	_commands[commands::rate_none]->enable = can_save_metadata;
	_commands[commands::rate_rejected]->enable = can_save_metadata;
	_commands[commands::refresh]->enable = true;
	// Refresh belongs to the batch view for as long as it is open, not just after a run: it always
	// means "re-plan from what is on disk now", and a button that appears only once is a button
	// nobody finds. A finished run keeps its results until it is pressed.
	const auto is_batch_view = view_mode == view_type::batch && _view_batch;
	_commands[commands::tool_refresh]->visible = is_batch_view;
	_commands[commands::tool_refresh]->enable = is_batch_view && _view_batch->can_refresh();
	_commands[commands::tool_run]->enable = !view_processing && ((view_mode == view_type::rename && _view_rename &&
		_view_rename->can_run()) || (view_mode == view_type::batch && _view_batch && _view_batch->can_run()));
	// One toolbar serves the rename view and every batch tool, so the run button must name the
	// operation the current view will actually perform.
	const auto run_text = (view_mode == view_type::batch && _view_batch)
		                      ? _view_batch->run_text().sv()
		                      : tt.command_rename_files.sv();
	update_toolbar_text(commands::tool_run, std::string(run_text));
	_commands[commands::tool_run]->text = run_text;
	_commands[commands::locate_run]->enable = view_mode == view_type::locate && _view_locate && _view_locate->can_run();
	_commands[commands::repeat_toggle]->enable = is_media_or_items_view;
	_commands[commands::search_related]->enable = is_media_or_items_view && is_displaying_item;
	_commands[commands::select_all]->enable = is_media_or_items_view;
	_commands[commands::select_invert]->enable = is_media_or_items_view;
	_commands[commands::select_nothing]->enable = is_media_or_items_view && has_selection;
	_commands[commands::show_raw_preview]->enable = is_media_or_items_view;
	_commands[commands::sort_date_created]->enable = is_media_or_items_view;
	_commands[commands::sort_date_original]->enable = is_media_or_items_view;
	_commands[commands::sort_date_modified]->enable = is_media_or_items_view;
	_commands[commands::sort_dates_ascending]->enable = is_media_or_items_view;
	_commands[commands::sort_dates_descending]->enable = is_media_or_items_view;
	_commands[commands::sort_def]->enable = is_media_or_items_view;
	_commands[commands::sort_name]->enable = is_media_or_items_view;
	_commands[commands::sort_size]->enable = is_media_or_items_view;
	_commands[commands::sync_analyze]->enable = view_mode == view_type::sync && !view_processing && _view_sync &&
		_view_sync->can_analyze();
	_commands[commands::sync_run]->enable = view_mode == view_type::sync && !view_processing && _view_sync &&
		_view_sync->can_run();
	_commands[commands::tags_run]->enable = view_mode == view_type::tags && !view_processing && _view_tags &&
		_view_tags->can_run();
	_commands[commands::tool_adjust_date]->enable = can_save_metadata;
	// A machine with no writer cannot burn at all, and design.md is explicit that a capability the
	// platform lacks is absent rather than dimmed - dimming would say "not now" forever.
	_commands[commands::tool_burn]->visible = has_burner;
	_commands[commands::tool_burn]->enable = has_selection && has_burner;
	_commands[commands::tool_convert]->enable = is_media_or_items_view && photos_only_result.success();
	_commands[commands::tool_copy_to_folder]->enable = can_process_local_items;
	_commands[commands::tool_delete]->enable = can_process_local_items;
	_commands[commands::tool_desktop_background]->enable = is_media_or_items_view && selection_status.showing_image;
	_commands[commands::tool_edit]->enable = is_media_or_items_view && _state.can_edit_media();
	_commands[commands::tool_edit_description]->enable = can_save_metadata;
	_commands[commands::tool_edit_metadata]->enable = can_save_metadata;
	_commands[commands::tool_share]->enable = can_process_local_files;
	// Ejecting a drive does not depend on what is being browsed, so it answers from every view.
	_commands[commands::tool_eject]->enable = true;
	_commands[commands::tool_file_properties]->enable = has_selection;
	_commands[commands::tool_import]->enable = is_media_or_items_view;
	_commands[commands::tool_locate]->enable = can_save_metadata;
	_commands[commands::tool_move_to_folder]->enable = can_process_local_items;

	const auto is_movie_view = view_mode == view_type::movie && _view_movie;
	// A timeline outlives the view, so clearing the selection must not lock the user out of the
	// movie they are part way through building.
	_commands[commands::tool_movie]->enable = is_media_or_items_view &&
		(has_movie_selection || (_view_movie && _view_movie->has_clips()));
	const auto movie_project_busy = is_movie_view && _view_movie->project_io_active();
	_commands[commands::tool_movie_add]->enable = is_movie_view && !movie_project_busy;
	_commands[commands::tool_movie_open]->enable = is_movie_view && !movie_project_busy;
	_commands[commands::tool_movie_import]->enable = is_movie_view && !movie_project_busy;
	_commands[commands::tool_movie_save]->enable = is_movie_view && !movie_project_busy && _view_movie->can_save_project();
	_commands[commands::tool_movie_remove]->enable = is_movie_view && _view_movie->has_clips();
	_commands[commands::tool_movie_send_to_end]->enable = is_movie_view && _view_movie->has_clips();
	_commands[commands::tool_movie_select_all]->enable = is_movie_view && _view_movie->has_clips();
	// Nothing to repair is not the same as a repair that is unavailable, so the offer appears only
	// once the probe has found a clip that is gone.
	_commands[commands::tool_movie_relink]->visible = is_movie_view && _view_movie->has_missing_clips();
	_commands[commands::tool_movie_relink]->enable = is_movie_view && !movie_project_busy && _view_movie->has_missing_clips();
	// A machine with no encoder cannot render at all, so Render is absent rather than dimmed, as
	// Burn is on a machine with no writer. The answer comes from the startup worker.
	const auto can_write_movies = _can_write_movies.value_or(false);
	_commands[commands::tool_movie_render]->visible = can_write_movies;
	_commands[commands::tool_movie_render]->enable = is_movie_view && !view_processing && can_write_movies &&
		_view_movie->can_render();
	_commands[commands::tool_new_folder]->enable = is_media_or_items_view && has_save_folder;
	_commands[commands::tool_open_with]->enable = has_selection;
	_commands[commands::tool_rename]->enable = can_process_local_items;
	_commands[commands::tool_rotate_anticlockwise]->enable = can_save_pixels;
	_commands[commands::tool_rotate_clockwise]->enable = can_save_pixels;
	_commands[commands::tool_rotate_reset]->enable = is_edit_view;
	_commands[commands::tool_save_current_video_frame]->enable = is_single_media_selection;
	_commands[commands::tool_scan]->enable = is_media_or_items_view;
	_commands[commands::tool_sync]->enable = is_media_or_items_view;
	_commands[commands::tool_tag]->enable = can_save_metadata;
	_commands[commands::verbose_metadata]->enable = is_media_or_items_view;
	// Close belongs to the task views, and Items to everything that is not already Items. Both are
	// tested by view alone: a command running in Items must not make either of them available.
	_commands[commands::view_close]->enable = view_mode != view_type::items && view_mode != view_type::media;
	// Cancel is a distinct command from Close: it stops the running task and leaves the view open.
	_commands[commands::view_cancel]->enable = view_processing;
	_commands[commands::view_cancel]->visible = view_processing;
	const auto close_text_changed = update_toolbar_text(commands::view_close, std::string(tt.command_close.sv()));
	_commands[commands::view_favorite_tags]->enable = is_media_or_items_view;
	_commands[commands::view_fullscreen]->enable = is_media_or_items_view;
	_commands[commands::view_help]->enable = true;
	_commands[commands::view_items]->enable = view_mode != view_type::items;
	_commands[commands::view_maximize]->enable = true;
	_commands[commands::view_minimize]->enable = true;
	_commands[commands::view_restore]->enable = true;
	_commands[commands::view_show_sidebar]->enable = is_items_view;
	_commands[commands::view_zoom]->enable = is_media_or_items_view && can_zoom;
	_commands[commands::view_zoom_fit]->enable = is_media_or_items_view && can_zoom;
	_commands[commands::view_zoom_fit_width]->enable = is_media_or_items_view && can_zoom;
	_commands[commands::view_zoom_fill]->enable = is_media_or_items_view && can_zoom;
	_commands[commands::view_zoom_toggle_fit]->enable = is_media_or_items_view && can_zoom;
	_commands[commands::view_zoom_in]->enable = is_media_or_items_view && can_zoom;
	_commands[commands::view_zoom_pane_flip]->enable = is_media_or_items_view && display && display->is_two() &&
		can_zoom;
	_commands[commands::view_zoom_out]->enable = is_media_or_items_view && can_zoom;
	_commands[commands::view_zoom_100]->enable = is_media_or_items_view && can_zoom;
	_commands[commands::menu_zoom]->enable = is_media_or_items_view && can_zoom;
	_commands[commands::menu_zoom_navigator]->enable = is_media_or_items_view && can_zoom;
	_commands[commands::view_zoom_navigator_auto_hide]->enable = is_media_or_items_view && can_zoom;
	_commands[commands::view_zoom_navigator_off]->enable = is_media_or_items_view && can_zoom;

	// A dimmed button is only honest if it says what would make it work again, so every command
	// gated on selection eligibility carries that test's own failure text while it is disabled.
	const auto set_disabled_reasons = [this, is_media_or_items_view](const df::process_result& result,
	                                                                 const std::initializer_list<commands> ids)
	{
		const auto reason = is_media_or_items_view && result.fail() ? result.to_string() : std::string{};

		for (const auto id : ids)
		{
			const auto found = _commands.find(id);
			if (found != _commands.end()) found->second->disabled_reason = found->second->enable
				                                                               ? std::string{}
				                                                               : reason;
		}
	};

	set_disabled_reasons(save_metadata_result, {
		                     commands::menu_rate_or_label, commands::rate_1, commands::rate_2, commands::rate_3,
		                     commands::rate_4, commands::rate_5, commands::rate_none, commands::rate_rejected,
		                     commands::label_approved, commands::label_none, commands::label_review,
		                     commands::label_second, commands::label_select, commands::label_to_do,
		                     commands::tool_adjust_date, commands::tool_edit_description, commands::tool_edit_metadata,
		                     commands::tool_locate, commands::tool_tag
	                     });
	set_disabled_reasons(save_pixels_result, {
		                     commands::tool_rotate_anticlockwise, commands::tool_rotate_clockwise
	                     });
	set_disabled_reasons(photos_only_result, {commands::tool_convert});
	set_disabled_reasons(local_files_result, {commands::print, commands::tool_share});
	set_disabled_reasons(local_items_result, {
		                     commands::tool_copy_to_folder, commands::tool_delete, commands::tool_move_to_folder,
		                     commands::tool_rename, commands::edit_copy, commands::edit_copy_item_path,
		                     commands::edit_cut
	                     });

	// Photo editing is singular, so its refusal is about the displayed item, not the whole selection.
	_commands[commands::tool_edit]->disabled_reason = _commands[commands::tool_edit]->enable
		                                                  ? std::string{}
		                                                  : std::string(tt.not_supported_photo_edit.sv());

	// Analyze refuses only because a path is missing, so it names the same cause the analysis used
	// to report after the fact. While a task is running the refusal is the task, not the paths.
	const auto analyze_reason = view_processing ? std::string{} : std::string(tt.platform_text.error_invalid_files.sv());

	for (const auto id : {commands::import_analyze, commands::sync_analyze})
	{
		const auto command = _commands[id];
		command->disabled_reason = command->enable ? std::string{} : analyze_reason;
	}

	// A dimmed Run is only honest if it names what would make it work again. The batch and rename
	// views keep that answer beside the test that produced it.
	_commands[commands::tool_run]->disabled_reason = view_mode == view_type::batch && _view_batch
		                                                ? _view_batch->run_blocked_reason()
		                                                : view_mode == view_type::rename && _view_rename
		                                                ? _view_rename->run_blocked_reason()
		                                                : std::string{};

	// Tags refuses a run over its own results for the same reason, and has no Refresh button, so the
	// reason is the only thing pointing at the edit that puts the view back into review.
	_commands[commands::tags_run]->disabled_reason = view_mode == view_type::tags && _view_tags && !
	                                                 view_processing && _view_tags->showing_results()
		                                                 ? std::string(tt.run_needs_refresh.sv())
		                                                 : std::string{};


	_commands[commands::playback_auto_play]->checked = setting.auto_play;
	_commands[commands::playback_auto_advance]->checked = setting.auto_advance;
	_commands[commands::playback_last_played_pos]->checked = setting.last_played_pos;
	_commands[commands::playback_repeat_one]->checked = setting.repeat == repeat_mode::repeat_one;
	_commands[commands::playback_repeat_all]->checked = setting.repeat == repeat_mode::repeat_all;
	_commands[commands::playback_repeat_none]->checked = setting.repeat == repeat_mode::repeat_none;
	_commands[commands::play]->checked = is_playing_media;
	_commands[commands::slideshow]->checked = is_slideshow;

	_commands[commands::pin_item]->checked = _state.has_pin();
	_commands[commands::option_highlight_large_items]->checked = setting.highlight_large_items;
	_commands[commands::sort_dates_descending]->checked = setting.sort_dates_descending;
	_commands[commands::sort_dates_ascending]->checked = !setting.sort_dates_descending;
	_commands[commands::view_show_sidebar]->checked = setting.show_sidebar;
	_commands[commands::option_scale_up]->checked = setting.scale_up;
	_commands[commands::option_show_rotated]->checked = setting.show_rotated;
	_commands[commands::verbose_metadata]->checked = setting.verbose_metadata;
	_commands[commands::show_raw_preview]->checked = setting.raw_preview;
	_commands[commands::view_zoom_navigator_auto_hide]->checked =
		setting.zoom_navigator == zoom_navigator_mode::auto_hide;
	_commands[commands::view_zoom_navigator_off]->checked = setting.zoom_navigator == zoom_navigator_mode::off;
	_commands[commands::view_zoom_fit]->checked = display && display->zoom_state().mode() == df::zoom_scale_mode::fit;
	_commands[commands::view_zoom_fit_width]->checked = display &&
		display->zoom_state().mode() == df::zoom_scale_mode::fit_width;
	_commands[commands::view_zoom_fill]->checked = display &&
		display->zoom_state().mode() == df::zoom_scale_mode::fill;
	_commands[commands::edit_item_preview]->checked = is_edit_view && _edit_view_state._preview_mode;
	_commands[commands::view_items]->checked = view_mode == view_type::items;
	_commands[commands::browse_recursive]->checked = _state.search().has_recursive_selector();
	_commands[commands::filter_photos]->checked = _state.filter().has_group(file_group::photo);
	_commands[commands::filter_videos]->checked = _state.filter().has_group(file_group::video);
	_commands[commands::filter_audio]->checked = _state.filter().has_group(file_group::audio);
	_commands[commands::large_font]->checked = setting.large_font;
	_commands[commands::view_fullscreen]->checked = _state.is_full_screen;
	_commands[commands::option_toggle_details]->checked = toggle_details_state;
	_commands[commands::view_favorite_tags]->checked = setting.sidebar.show_favorite_tags_only;

	_commands[commands::group_album]->checked = _state.group_order() == group_by::album_show;
	_commands[commands::group_aspect_ratio]->checked = _state.group_order() == group_by::aspect_ratio;
	_commands[commands::group_camera]->checked = _state.group_order() == group_by::camera;
	_commands[commands::group_created]->checked = _state.group_order() == group_by::date_created;
	_commands[commands::group_original]->checked = _state.group_order() == group_by::date_original;
	_commands[commands::group_presence]->checked = _state.group_order() == group_by::presence;
	_commands[commands::group_file_type]->checked = _state.group_order() == group_by::file_type;
	_commands[commands::group_location]->checked = _state.group_order() == group_by::location;
	_commands[commands::group_modified]->checked = _state.group_order() == group_by::date_modified;
	_commands[commands::group_pixels]->checked = _state.group_order() == group_by::resolution;
	_commands[commands::group_rating]->checked = _state.group_order() == group_by::rating_label;
	_commands[commands::group_shuffle]->checked = _state.group_order() == group_by::shuffle;
	_commands[commands::group_size]->checked = _state.group_order() == group_by::size;
	_commands[commands::group_extension]->checked = _state.group_order() == group_by::extension;
	_commands[commands::group_folder]->checked = _state.group_order() == group_by::folder;
	_commands[commands::sort_def]->checked = _state.sort_order() == sort_by::def;
	_commands[commands::sort_name]->checked = _state.sort_order() == sort_by::name;
	_commands[commands::sort_size]->checked = _state.sort_order() == sort_by::size;
	_commands[commands::sort_date_created]->checked = _state.sort_order() == sort_by::date_created;
	_commands[commands::sort_date_original]->checked = _state.sort_order() == sort_by::date_original;
	_commands[commands::sort_date_modified]->checked = _state.sort_order() == sort_by::date_modified;
	_commands[commands::english]->checked = setting.language == "en";


	_commands[commands::playback_volume200]->checked = setting.media_volume == media_volume_boost;
	_commands[commands::playback_volume100]->checked = setting.media_volume == media_volumes[0];
	_commands[commands::playback_volume75]->checked = setting.media_volume == media_volumes[1];
	_commands[commands::playback_volume50]->checked = setting.media_volume == media_volumes[2];
	_commands[commands::playback_volume25]->checked = setting.media_volume == media_volumes[3];
	_commands[commands::playback_volume0]->checked = setting.media_volume == media_volumes[4];

	_commands[commands::play]->icon = is_playing_media ? icon_index::pause : icon_index::play;
	_commands[commands::slideshow]->icon = is_slideshow ? icon_index::pause : icon_index::slideshow;
	_commands[commands::view_fullscreen]->icon = _state.is_full_screen
		                                             ? icon_index::fullscreen_exit
		                                             : icon_index::fullscreen;
	_commands[commands::playback_volume_toggle]->icon = sound_icon();
	_commands[commands::repeat_toggle]->icon = repeat_toggle_icon();
	_commands[commands::favorite]->icon = _state.search_is_favorite() ? icon_index::star_solid : icon_index::star;
	_commands[commands::options_collection]->icon = icon_index::media_options;


	const auto summary_text = format_items_summary(_state.effective_group_order(), _state.sort_order(),
	                                               _state.summary_shown(),
	                                               _state.item_index.is_init_complete());

	update_toolbar_text(commands::menu_group_toolbar, summary_text);
	update_toolbar_text(commands::filter_photos, str::format_count(_state.count_total(file_group::photo), true));
	update_toolbar_text(commands::filter_videos, str::format_count(_state.count_total(file_group::video), true));
	update_toolbar_text(commands::filter_audio, str::format_count(_state.count_total(file_group::audio), true));

	_navigate1->update_button_state(resize, false);
	_navigate2->update_button_state(resize, false);
	_navigate3->update_button_state(resize, false);
	_search_edit->set_icon(address_icon(_state.search()));

	_media_edit_commands->update_button_state(resize, close_text_changed);
	_import_commands->update_button_state(resize, close_text_changed);
	_locate_commands->update_button_state(resize, close_text_changed);
	_tool_commands->update_button_state(resize, close_text_changed);
	_sync_commands->update_button_state(resize, close_text_changed);
	_tags_commands->update_button_state(resize, close_text_changed);
	_movie_commands->update_button_state(resize, close_text_changed);

	const view_element_event e{view_element_event_type::update_command_state, _view_frame};
	_view->broadcast_event(e);
}

recti app_frame::calc_search_popup_bounds() const
{
	const auto edit_bounds = _search_edit->window_bounds();
	const auto height = _search_predictions_frame ? _search_predictions_frame->height() : 320;
	return {edit_bounds.left + 8, edit_bounds.bottom, edit_bounds.right - 8, edit_bounds.bottom + height};
}


// The `keys::` values are runtime globals filled in by the platform layer, so this is a function
// rather than a constant table.
std::vector<command_accelerator> default_keyboard_accelerators()
{
	constexpr auto control = keyboard_accelerator_t::control;
	constexpr auto shift = keyboard_accelerator_t::shift;
	constexpr auto alt = keyboard_accelerator_t::alt;

	return {
		{commands::rate_none, {'0'}},
		{commands::rate_1, {'1'}},
		{commands::rate_2, {'2'}},
		{commands::rate_3, {'3'}},
		{commands::rate_4, {'4'}},
		{commands::rate_5, {'5'}},
		{commands::rate_rejected, {keys::DEL, alt}},
		{commands::label_select, {'6'}},
		{commands::label_second, {'7'}},
		{commands::label_approved, {'8'}},
		{commands::label_review, {'9'}},

		{commands::pin_item, {'A'}},
		{commands::select_all, {'A', control}},
		{commands::tool_edit_metadata, {'E', control}},
		{commands::tool_copy_to_folder, {'C', shift | control}},
		{commands::edit_copy, {'C', control}},
		{commands::select_invert, {'D', control}},
		{commands::tool_adjust_date, {'D', shift | control}},
		{commands::tool_edit, {'E'}},
		{commands::tool_eject, {'E', shift | control}},
		{commands::browse_search, {'F', control}},
		{commands::option_toggle_details, {'K'}},
		{commands::tool_locate, {'L'}},
		{commands::repeat_toggle, {'L', control}},
		{commands::rate_rejected, {'M'}},
		{commands::tool_new_folder, {'N', control}},
		{commands::view_items, {'N', shift | control}},
		{commands::browse_open_containingfolder, {'O', shift | control}},
		{commands::label_to_do, {'P'}},
		{commands::search_related, {'R'}},
		{commands::tool_tag, {'T'}},
		{commands::tool_save_current_video_frame, {'S', control}},
		{commands::select_nothing, {'U', control}},
		{commands::edit_paste, {'V', control}},
		{commands::edit_cut, {'X', control}},
		{commands::tool_move_to_folder, {'X', shift | control}},
		{commands::browse_back, {keys::BACK}},
		{commands::tool_delete, {keys::DEL}},
		{commands::edit_cut, {keys::DEL, shift}},
		{commands::group_toggle, {keys::DOWN, alt}},
		{commands::group_file_type, {'K', control}},
		{commands::group_created, {'K', control | shift}},

		{commands::view_help, {keys::F1}},
		{commands::keyboard, {keys::F1, control}},
		{commands::tool_rename, {keys::F2}},
		{commands::browse_search, {keys::F3}},
		{commands::browse_search, {keys::BROWSER_SEARCH}},
		{commands::filter_items, {keys::F3, control}},
		{commands::view_show_sidebar, {keys::F4}},
		{commands::view_show_sidebar, {keys::BROWSER_FAVORITES}},
		{commands::refresh, {keys::F5}},
		{commands::options_general, {keys::F6}},
		{commands::options_sidebar, {keys::F6, shift | control}},
		{commands::options_collection, {keys::F6, control}},
		{commands::playback_auto_play, {keys::F7, control}},
		{commands::playback_volume_toggle, {keys::F7}},
		{commands::tool_convert, {keys::F8}},
		{commands::tool_import, {keys::F9}},
		{commands::browse_recursive, {keys::F9, control}},
		{commands::tool_sync, {keys::F9, shift | control}},
		{commands::tool_share, {keys::F10}},
		{commands::option_scale_up, {keys::F11, shift | control}},
		{commands::view_fullscreen, {keys::F11}},
		{commands::view_fullscreen, {keys::SPACE, shift | control}},
		{commands::edit_copy, {keys::INSERT, control}},
		{commands::edit_paste, {keys::INSERT, shift}},
		{commands::browse_back, {keys::LEFT, alt}},
		{commands::browse_previous_folder, {keys::LEFT, alt | control}},
		{commands::browse_previous_item_extend, {keys::LEFT, control}},
		{commands::tool_rotate_anticlockwise, {keys::OEM_4}},
		{commands::tool_rotate_clockwise, {keys::OEM_6}},
		{commands::tool_file_properties, {keys::RETURN, shift | control}},
		{commands::browse_open_in_file_browser, {keys::RETURN, shift}},
		{commands::tool_open_with, {keys::RETURN, control}},
		{commands::view_zoom, {keys::SPACE, control}},
		{commands::view_zoom_pane_flip, {keys::TAB}},
		{commands::play, {keys::SPACE}},
		{commands::slideshow, {keys::SPACE, shift}},
		{commands::browse_forward, {keys::RIGHT, alt}},
		{commands::browse_next_folder, {keys::RIGHT, alt | control}},
		{commands::browse_next_item_extend, {keys::RIGHT, control}},
		{commands::browse_parent, {keys::UP, alt}},
		{commands::browse_previous_group, {keys::PRIOR}},
		{commands::browse_next_group, {keys::NEXT}},

		// Only reachable while the edit view is open, which is why these repeat Alt+Left/Alt+Right.
		{commands::edit_item_save_and_prev, {keys::LEFT, alt}},
		{commands::edit_item_save_and_next, {keys::RIGHT, alt}},
		{commands::edit_item_save, {keys::RETURN, alt}},
		{commands::view_close, {keys::ESCAPE}},
	};
}

void app_frame::update_command_text()
{
	def_command(commands::tool_adjust_date, command_group::tools, icon_index::time, tt.command_adjust_date);
	def_command(commands::tool_edit_description, command_group::tools, icon_index::edit_metadata,
	            tt.command_edit_metadata);
	def_command(commands::tool_edit_metadata, command_group::tools, icon_index::edit_metadata,
	            tt.command_edit_metadata);
	def_command(commands::exit, command_group::help, icon_index::close, tt.command_app_exit);
	def_command(commands::playback_auto_play, command_group::media_playback, icon_index::play, tt.command_autoplay);
	def_command(commands::playback_auto_advance, command_group::options, icon_index::next_image,
	            tt.command_auto_advance, tt.auto_advance_help);
	def_command(commands::playback_last_played_pos, command_group::media_playback, icon_index::none,
	            tt.command_last_played_pos);
	def_command(commands::browse_back, command_group::navigation, icon_index::back, tt.command_browse_back);
	def_command(commands::browse_forward, command_group::navigation, icon_index::next, tt.command_browse_forward);
	def_command(commands::browse_next_folder, command_group::navigation, icon_index::right,
	            tt.command_browse_next_folder);
	def_command(commands::browse_next_group, command_group::selection, icon_index::none, tt.command_browse_next_group);
	def_command(commands::browse_next_item, command_group::selection, icon_index::right,
	            tt.command_browse_next_item);
	def_command(commands::browse_next_item_extend, command_group::selection, icon_index::right,
	            tt.command_browse_next_item_extend);
	def_command(commands::browse_parent, command_group::navigation, icon_index::parent, tt.command_browse_parent);
	def_command(commands::browse_previous_folder, command_group::navigation, icon_index::left,
	            tt.command_browse_previous_folder);
	def_command(commands::browse_previous_group, command_group::selection, icon_index::none,
	            tt.command_browse_previous_group);
	def_command(commands::browse_previous_item, command_group::selection, icon_index::left,
	            tt.command_browse_previous_item);
	def_command(commands::browse_previous_item_extend, command_group::selection, icon_index::left,
	            tt.command_browse_previous_item_extend);
	def_command(commands::tool_burn, command_group::tools, icon_index::disk, tt.command_burn);
	def_command(commands::tool_save_current_video_frame, command_group::tools, icon_index::photo, tt.command_capture);
	def_command(commands::view_close, command_group::none, icon_index::close, tt.command_close);
	def_command(commands::edit_item_color_reset, command_group::none, icon_index::undo, tt.command_color_reset,
	            tt.tooltip_color_reset);
	def_command(commands::tool_convert, command_group::tools, icon_index::convert, tt.command_convert_or_resize);
	def_command(commands::tool_copy_to_folder, command_group::file_management, icon_index::copy_to_folder,
	            tt.command_copy);
	def_command(commands::tool_delete, command_group::file_management, icon_index::del, tt.command_delete);
	def_command(commands::tool_desktop_background, command_group::tools, icon_index::wallpaper,
	            tt.command_desktop_background);
	def_command(commands::menu_display_options, command_group::none, icon_index::none, tt.command_display_options);
	def_command(commands::tool_edit, command_group::tools, icon_index::edit, tt.command_edit,
	            tt.tooltip_edit1);
	def_command(commands::tool_movie, command_group::tools, icon_index::video, tt.command_movie, tt.tooltip_movie);
	def_command(commands::tool_movie_render, command_group::tools, icon_index::movies, tt.command_movie_render,
	            tt.tooltip_movie_render);
	def_command(commands::tool_movie_add, command_group::tools, icon_index::add, tt.command_movie_add);
	def_command(commands::tool_movie_remove, command_group::tools, icon_index::del, tt.command_movie_remove);
	def_command(commands::tool_movie_relink, command_group::tools, icon_index::link, tt.command_movie_relink,
	            tt.tooltip_movie_relink);
	def_command(commands::tool_movie_open, command_group::tools, icon_index::folder, tt.command_movie_open);
	def_command(commands::tool_movie_save, command_group::tools, icon_index::save, tt.command_movie_save);
	def_command(commands::tool_movie_import, command_group::tools, icon_index::import, tt.command_movie_import);
	def_command(commands::tool_movie_send_to_end, command_group::tools, icon_index::right,
	            tt.command_movie_send_to_end);
	def_command(commands::tool_movie_select_all, command_group::tools, icon_index::none, tt.command_select_all);
	def_command(commands::edit_copy, command_group::file_management, icon_index::edit_copy, tt.command_edit_copy);
	def_command(commands::edit_copy_item_path, command_group::file_management, icon_index::edit_copy,
	            tt.command_edit_copy_item_path);
	def_command(commands::edit_cut, command_group::file_management, icon_index::edit_cut, tt.command_edit_cut);
	def_command(commands::edit_paste, command_group::file_management, icon_index::edit_paste, tt.command_edit_paste);
	def_command(commands::tool_eject, command_group::file_management, icon_index::eject, tt.command_eject);
	def_command(commands::tool_file_properties, command_group::file_management, icon_index::document_information,
	            tt.command_file_properties);
	def_command(commands::browse_search, command_group::navigation, icon_index::search, tt.command_file_search);
	def_command(commands::browse_recursive, command_group::navigation, icon_index::recursive, tt.command_flatten);
	def_command(commands::view_fullscreen, command_group::media_playback, icon_index::fullscreen, tt.command_fullscreen,
	            tt.tooltip_fullscreen);
	def_command(commands::option_highlight_large_items, command_group::options, icon_index::none,
	            tt.command_highlight_large_items);
	def_command(commands::tool_import, command_group::tools, icon_index::import, tt.command_import);
	def_command(commands::tool_sync, command_group::tools, icon_index::sync, tt.command_sync);
	def_command(commands::options_collection, command_group::options, icon_index::set, tt.command_collection_options);
	def_command(commands::keyboard, command_group::help, icon_index::keyboard, tt.command_keyboard);
	def_command(commands::tool_locate, command_group::tools, icon_index::location, tt.command_locate);
	def_command(commands::view_maximize, command_group::help, icon_index::maximize, tt.command_maximize);
	def_command(commands::view_minimize, command_group::help, icon_index::minimize, tt.command_minimize);
	def_command(commands::tool_move_to_folder, command_group::file_management, icon_index::move_to_folder,
	            tt.command_move);
	def_command(commands::view_show_sidebar, command_group::options, icon_index::navigation, tt.command_nav_bar,
	            tt.tooltip_nav_bar);
	def_command(commands::menu_navigate, command_group::none, icon_index::none, tt.command_navigate);
	def_command(commands::tool_new_folder, command_group::file_management, icon_index::new_folder,
	            tt.command_new_folder);
#ifndef WINSTORE
	def_command(commands::info_new_version, command_group::none, icon_index::lightbulb, tt.command_new_version);
	def_command(commands::info_check_for_updates, command_group::help, icon_index::lightbulb,
	            tt.command_check_for_updates);
#endif
	def_command(commands::menu_open, command_group::none, icon_index::open_one, tt.command_open, tt.tooltip_open);
	def_command(commands::browse_open_containingfolder, command_group::navigation, icon_index::none,
	            tt.command_show_in_folder);
	def_command(commands::browse_open_googlemap, command_group::open, icon_index::location, tt.command_open_google_map);
	def_command(commands::browse_open_in_file_browser, command_group::open, icon_index::none,
	            tt.command_show_in_file_browser);
	def_command(commands::tool_open_with, command_group::open, icon_index::open_one, tt.command_open_with);
	def_command(commands::options_general, command_group::options, icon_index::settings, tt.command_options);
	def_command(commands::pin_item, command_group::selection, icon_index::pin, tt.command_pin, tt.tooltip_pin);
	def_command(commands::play, command_group::media_playback, icon_index::play, tt.command_play, tt.tooltip_play);
	def_command(commands::slideshow, command_group::media_playback, icon_index::slideshow, tt.command_slideshow,
	            tt.tooltip_slideshow);
	def_command(commands::print, command_group::tools, icon_index::print, tt.command_print);
	def_command(commands::rate_none, command_group::rate_flag, icon_index::none, tt.command_rate_0);
	def_command(commands::rate_1, command_group::rate_flag, icon_index::none, tt.command_rate_1);
	def_command(commands::rate_2, command_group::rate_flag, icon_index::none, tt.command_rate_2);
	def_command(commands::rate_3, command_group::rate_flag, icon_index::none, tt.command_rate_3);
	def_command(commands::rate_4, command_group::rate_flag, icon_index::none, tt.command_rate_4);
	def_command(commands::rate_5, command_group::rate_flag, icon_index::none, tt.command_rate_5);
	def_command(commands::rate_rejected, command_group::rate_flag, rate_label_reject.icon, tt.command_rate_rejected);
	def_command(commands::label_approved, command_group::rate_flag, rate_label_approved.icon,
	            tt.command_label_approved);
	def_command(commands::label_to_do, command_group::rate_flag, rate_label_to_do.icon, tt.command_label_to_do);
	def_command(commands::label_select, command_group::rate_flag, rate_label_select.icon, tt.command_label_select);
	def_command(commands::label_review, command_group::rate_flag, rate_label_review.icon, tt.command_label_review);
	def_command(commands::label_second, command_group::rate_flag, rate_label_second.icon, tt.command_label_second);
	def_command(commands::label_none, command_group::rate_flag, icon_index::none, tt.command_label_none);
	def_command(commands::refresh, command_group::navigation, icon_index::refresh, tt.command_refresh);
	def_command(commands::search_related, command_group::tools, icon_index::compare, tt.command_related,
	            tt.tooltip_related);
	def_command(commands::tool_rename, command_group::file_management, icon_index::rename, tt.command_rename);
	def_command(commands::playback_repeat_none, command_group::options, icon_index::repeat_none, tt.command_repeat_none,
	            tt.repeat_off_help);
	def_command(commands::playback_repeat_one, command_group::options, icon_index::repeat_one, tt.command_repeat_one,
	            tt.repeat_one_help);
	def_command(commands::playback_repeat_all, command_group::options, icon_index::repeat_all, tt.command_repeat_all,
	            tt.repeat_help);
	def_command(commands::playback_menu, command_group::media_playback, icon_index::media_options,
	            tt.command_playback_menu);
	def_command(commands::menu_playback, command_group::media_playback, icon_index::media_options,
	            tt.command_playback_toolbar, tt.command_playback_menu);
	def_command(commands::repeat_toggle, command_group::media_playback, icon_index::repeat_all,
	            tt.command_repeat_toggle);
	def_command(commands::view_restore, command_group::help, icon_index::restore, tt.command_restore);
	def_command(commands::tool_rotate_anticlockwise, command_group::tools, icon_index::rotate_anticlockwise,
	            tt.command_rotate_anticlockwise);
	def_command(commands::tool_rotate_clockwise, command_group::tools, icon_index::rotate_clockwise,
	            tt.command_rotate_clockwise);
	def_command(commands::tool_rotate_reset, command_group::none, icon_index::undo, tt.command_rotate_reset,
	            tt.tooltip_rotate_reset);
	def_command(commands::edit_item_save, command_group::edit_item, icon_index::save, tt.command_save);
	def_command(commands::edit_item_auto_color, command_group::edit_item, icon_index::lightbulb, tt.command_auto_color,
	            tt.tooltip_auto_color);
	def_command(commands::edit_item_auto_document, command_group::edit_item, icon_index::scan_text, tt.command_auto_document,
	            tt.tooltip_auto_document);
	def_command(commands::edit_item_auto_straighten, command_group::edit_item, icon_index::lightbulb,
	            tt.command_auto_straighten, tt.tooltip_auto_straighten);
	def_command(commands::edit_item_preview, command_group::edit_item, icon_index::preview,
	            tt.command_edit_preview, tt.tooltip_edit_preview);
	def_command(commands::edit_item_save_and_prev, command_group::edit_item, icon_index::back_image,
	            tt.command_save_and_back, tt.command_save_and_back_tooltip);
	def_command(commands::edit_item_save_and_next, command_group::edit_item, icon_index::next_image,
	            tt.command_save_and_next, tt.command_save_and_next_tooltip);
	def_command(commands::edit_item_save_as, command_group::edit_item, icon_index::save_copy, tt.command_save_as);
	def_command(commands::option_scale_up, command_group::options, icon_index::fit, tt.command_scale_up,
	            tt.tooltip_scale_up);
	def_command(commands::tool_scan, command_group::tools, icon_index::scanner, tt.command_scan);
	def_command(commands::options_sidebar, command_group::options, icon_index::none, tt.command_customise);
	def_command(commands::favorite_tags, command_group::options, icon_index::tag, tt.customise_tags_title);
	def_command(commands::select_all, command_group::selection, icon_index::none, tt.command_select_all);
	def_command(commands::select_invert, command_group::selection, icon_index::none, tt.command_select_invert);
	def_command(commands::select_nothing, command_group::selection, icon_index::none, tt.command_select_nothing);
	// Named for what it will do on this system: the share sheet, or an email draft where there is none.
	if (platform::can_share_files())
	{
		def_command(commands::tool_share, command_group::tools, icon_index::share, tt.command_share);
	}
	else
	{
		def_command(commands::tool_share, command_group::tools, icon_index::mail, tt.command_share_email);
	}
	def_command(commands::option_show_rotated, command_group::options, icon_index::orientation, tt.item_oriented);
	def_command(commands::verbose_metadata, command_group::options, icon_index::verbose_metadata,
	            tt.show_verbose_metadata);
	def_command(commands::show_raw_preview, command_group::options, icon_index::preview, tt.preview_show_preview);
	def_command(commands::tool_tag, command_group::tools, icon_index::tag, tt.prop_name_tag, tt.tooltip_tag_with);
	def_command(commands::menu_language, command_group::none, icon_index::language, tt.command_language,
	            tt.tooltip_language);
	def_command(commands::english, command_group::none, icon_index::none, "English", "English language");
	def_command(commands::option_toggle_details, command_group::options, icon_index::details, tt.command_toggle_details,
	            tt.tooltip_toggle_details_all);
	def_command(commands::option_toggle_item_size, command_group::options, icon_index::zoom_in,
	            tt.command_toggle_item_size);
	def_command(commands::menu_tools_toolbar, command_group::none, icon_index::tools, tt.command_tools,
	            tt.tooltip_tools);
	def_command(commands::menu_tools, command_group::none, icon_index::tools, tt.command_tools, tt.tooltip_tools);
	def_command(commands::view_help, command_group::help, icon_index::question, tt.command_view_help);
	def_command(commands::view_items, command_group::options, icon_index::items, tt.command_view_items);
	def_command(commands::large_font, command_group::options, icon_index::none, tt.command_view_large_font);
	def_command(commands::view_favorite_tags, command_group::options, icon_index::tag, tt.command_favorite_tags);
	def_command(commands::menu_main, command_group::none, icon_index::more, tt.command_view_menu, tt.tooltip_view_menu);
	def_command(commands::menu_options, command_group::none, icon_index::settings, tt.options_title);
	def_command(commands::menu_rate_or_label, command_group::none, icon_index::none, tt.command_view_rate_label);
	def_command(commands::menu_select, command_group::none, icon_index::none, tt.command_view_select);
	def_command(commands::menu_group_toolbar, command_group::none, icon_index::group, tt.command_view_sort);
	def_command(commands::filter_items, command_group::navigation, icon_index::search, tt.command_filter_items);
	def_command(commands::filter_photos, command_group::selection, icon_index::photo, tt.command_filter_photos);
	def_command(commands::filter_videos, command_group::selection, icon_index::video, tt.command_filter_videos);
	def_command(commands::filter_audio, command_group::selection, icon_index::audio, tt.command_filter_audio);
	def_command(commands::menu_group, command_group::none, icon_index::group, tt.command_menu_group_sort);
	def_command(commands::playback_volume200, command_group::media_playback, icon_index::volume3, tt.command_volume200);
	def_command(commands::playback_volume100, command_group::media_playback, icon_index::volume3, tt.command_volume100);
	def_command(commands::playback_volume75, command_group::media_playback, icon_index::volume2, tt.command_volume75);
	def_command(commands::playback_volume50, command_group::media_playback, icon_index::volume1, tt.command_volume50);
	def_command(commands::playback_volume25, command_group::media_playback, icon_index::volume0, tt.command_volume25);
	def_command(commands::playback_volume0, command_group::media_playback, icon_index::mute, tt.command_volume0);
	def_command(commands::playback_volume_toggle, command_group::media_playback, icon_index::volume3,
	            tt.command_toggle_volume);
	def_command(commands::view_zoom, command_group::media_playback, icon_index::zoom_in, tt.command_zoom);
	def_command(commands::view_zoom_fit, command_group::media_playback, icon_index::fit, tt.command_zoom_fit);
	def_command(commands::view_zoom_fit_width, command_group::media_playback, icon_index::fit,
	            tt.command_zoom_fit_width);
	def_command(commands::view_zoom_fill, command_group::media_playback, icon_index::fit, tt.command_zoom_fill);
	def_command(commands::view_zoom_toggle_fit, command_group::media_playback, icon_index::fit, tt.command_zoom_fit);
	def_command(commands::view_zoom_in, command_group::media_playback, icon_index::zoom_in, tt.command_zoom_in);
	def_command(commands::view_zoom_pane_flip, command_group::media_playback, icon_index::swap, tt.command_zoom_flip);
	def_command(commands::view_zoom_out, command_group::media_playback, icon_index::zoom_out, tt.command_zoom_out);
	def_command(commands::view_zoom_100, command_group::media_playback, icon_index::one_to_one, tt.command_zoom_100);
	def_command(commands::menu_zoom, command_group::media_playback, icon_index::overview,
	            tt.command_zoom_presets);
	def_command(commands::menu_zoom_navigator, command_group::media_playback, icon_index::overview,
	            tt.command_zoom_navigator);
	def_command(commands::view_zoom_navigator_auto_hide, command_group::media_playback, icon_index::none,
	            tt.command_zoom_navigator_auto_hide);
	def_command(commands::view_zoom_navigator_off, command_group::media_playback, icon_index::none,
	            tt.command_zoom_navigator_off);
	def_command(commands::favorite, command_group::navigation, icon_index::star, tt.command_favorite);
	def_command(commands::advanced_search, command_group::navigation, icon_index::search, tt.command_advanced_search);
	def_command(commands::group_album, command_group::group_by, icon_index::none, tt.command_group_album);
	def_command(commands::group_aspect_ratio, command_group::group_by, icon_index::none, tt.command_group_aspect_ratio);
	def_command(commands::group_presence, command_group::group_by, icon_index::none, tt.command_group_presence);
	def_command(commands::group_camera, command_group::group_by, icon_index::none, tt.command_group_camera);
	def_command(commands::group_created, command_group::group_by, icon_index::none, tt.command_group_created);
	def_command(commands::group_original, command_group::group_by, icon_index::none, tt.command_group_original);
	def_command(commands::group_file_type, command_group::group_by, icon_index::none, tt.command_group_file_type);
	def_command(commands::group_location, command_group::group_by, icon_index::none, tt.command_group_location);
	def_command(commands::group_modified, command_group::group_by, icon_index::none, tt.command_group_modified);
	def_command(commands::group_pixels, command_group::group_by, icon_index::none, tt.command_group_resolution);
	def_command(commands::group_rating, command_group::group_by, icon_index::none, tt.command_group_rating);
	def_command(commands::group_shuffle, command_group::group_by, icon_index::none, tt.command_group_shuffle);
	def_command(commands::group_size, command_group::group_by, icon_index::none, tt.command_group_size);
	def_command(commands::group_extension, command_group::group_by, icon_index::none, tt.command_group_extension);
	def_command(commands::group_folder, command_group::group_by, icon_index::none, tt.command_group_folder);
	def_command(commands::group_toggle, command_group::group_by, icon_index::none, tt.command_toggle_group_by);
	def_command(commands::sort_dates_descending, command_group::options, icon_index::none,
	            tt.command_sort_dates_descending);
	def_command(commands::sort_dates_ascending, command_group::options, icon_index::none,
	            tt.command_sort_dates_ascending);
	def_command(commands::sort_name, command_group::sort_by, icon_index::none, tt.command_sort_name);
	def_command(commands::sort_size, command_group::sort_by, icon_index::none, tt.command_sort_size);
	def_command(commands::sort_def, command_group::sort_by, icon_index::none, tt.command_sort_def);
	def_command(commands::sort_date_created, command_group::sort_by, icon_index::none, tt.command_sort_date_created);
	def_command(commands::sort_date_original, command_group::sort_by, icon_index::none, tt.command_sort_date_original);
	def_command(commands::sort_date_modified, command_group::sort_by, icon_index::none, tt.command_sort_date_modified);
	def_command(commands::sync_analyze, command_group::none, icon_index::refresh, tt.analyze);
	def_command(commands::sync_run, command_group::none, icon_index::play, tt.command_sync);
	def_command(commands::tags_run, command_group::none, icon_index::tag, tt.command_apply_tags);
	def_command(commands::view_cancel, command_group::none, icon_index::cancel, tt.command_cancel_operation);
	def_command(commands::tool_run, command_group::none, icon_index::play, tt.command_rename_files);
	def_command(commands::tool_refresh, command_group::none, icon_index::refresh, tt.command_refresh);
	def_command(commands::import_analyze, command_group::none, icon_index::refresh, tt.analyze);
	def_command(commands::import_run, command_group::none, icon_index::play, tt.command_import);
	def_command(commands::locate_run, command_group::none, icon_index::play, tt.command_locate);

	for (const auto& [id, key] : default_keyboard_accelerators())
	{
		_commands[id]->kba.emplace_back(key);
	}

	for (const auto& c : _commands)
	{
		c.second->keyboard_accelerator_text = format_keyboard_accelerator(c.second->kba);
	}

	// Left and Right are handled directly by the view rather than by the accelerator table, so
	// their hints are assigned after the formatting pass that would otherwise clear them.
	_commands[commands::browse_previous_item]->keyboard_accelerator_text = tt.keyboard_left;
	_commands[commands::browse_next_item]->keyboard_accelerator_text = tt.keyboard_right;

	for (const auto id : {
		     commands::menu_open, commands::menu_tools_toolbar, commands::menu_playback, commands::view_close,
		     commands::sync_analyze, commands::sync_run, commands::tool_run, commands::import_analyze,
		     commands::import_run, commands::locate_run, commands::tags_run, commands::view_cancel,
		     commands::tool_refresh,
		     commands::edit_item_save, commands::edit_item_save_and_prev, commands::edit_item_save_and_next,
		     commands::edit_item_save_as, commands::edit_item_preview,
		     commands::tool_movie_add, commands::tool_movie_remove, commands::tool_movie_open,
		     commands::tool_movie_save, commands::tool_movie_import,
		     commands::tool_movie_render, commands::tool_movie_relink,
		     commands::menu_group_toolbar, commands::filter_photos, commands::filter_videos,
		     commands::filter_audio
	     })
	{
		_commands[id]->toolbar_text = _commands[id]->text;
	}
}


command_info_ptr app_frame::find_or_create_command_info(const commands id)
{
	const auto found = _commands.find(id);
	if (found != _commands.end()) return found->second;
	return _commands[id] = std::make_shared<ui::command>();
}

void app_frame::add_command_invoke(const commands id, std::function<void()> invoke)
{
	find_or_create_command_info(id)->invoke = std::move(invoke);
}

void app_frame::def_command(const commands id, const command_group group, const icon_index icon,
                            const std::string_view text, const std::string_view tooltip)
{
	const auto c = find_or_create_command_info(id);
	c->group = group;
	c->icon = icon;
	c->text = text;
	c->tooltip_text = tooltip;
	c->kba.clear();
	c->keyboard_accelerator_text.clear();
}

ui::command_ptr app_frame::find_command(const commands id) const
{
	const auto it = _commands.find(id);

	if (it != _commands.cend())
	{
		return it->second;
	}

	return nullptr;
}

void app_frame::tooltip(view_hover_element& hover, const commands id) const
{
	const auto command = find_command(id);

	if (command)
	{
		if (command->icon != icon_index::none)
		{
			hover.elements->add(make_icon_element(command->icon, flex_item::no_break));
		}

		if (!command->text.empty())
		{
			hover.elements->add(std::make_shared<text_element>(command->text, ui::style::font_face::dialog,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::line_break));
		}

		if (!command->tooltip_text.empty())
		{
			hover.elements->add(std::make_shared<text_element>(command->tooltip_text, ui::style::font_face::dialog,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::line_break));
		}

		if (!command->enable && !command->disabled_reason.empty())
		{
			hover.elements->add(std::make_shared<text_element>(command->disabled_reason, ui::style::font_face::dialog,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::line_break | view_element_style::important));
		}
	}

	std::string keyboard_accelerator;

	if (id == commands::browse_previous_item || id == commands::browse_next_item)
	{
		const auto forward = id == commands::browse_next_item;
		const auto i = _state.next_item(forward, false);

		if (i)
		{
			const auto surface = i->thumbnail_surface();

			if (is_valid(surface))
			{
				hover.elements->add(
					std::make_shared<surface_element>(surface, 200, flex_item::center | flex_item::new_line));
			}
			else if (is_valid(i->thumbnail()))
			{
				i->stage_thumbnail_surface(_state._async, false, true);
			}

			hover.elements->add(std::make_shared<text_element>(i->name()));
		}

		keyboard_accelerator = forward ? tt.keyboard_right : tt.keyboard_left;
	}
	else if (id == commands::menu_group_toolbar)
	{
		hover.elements->clear();
		hover.elements->add(make_icon_element(icon_index::group, flex_item::no_break));

		// locations.md 7.1: the breakdown belongs to the totals affordance now; repeating it here
		// would put it back where a user reaching for grouping does not want it.
		hover.elements->add(std::make_shared<text_element>(tt.group_sort_tooltip, ui::style::font_face::dialog,
		                                                   ui::style::text_style::multiline,
		                                                   flex_item::line_break));
	}
	else if (id == commands::browse_open_containingfolder)
	{
		const auto i = _state.command_item();

		if (i)
		{
			hover.elements->add(std::make_shared<text_element>(i->containing().text()));
		}
	}
	else if (id == commands::browse_next_folder || id == commands::browse_previous_folder)
	{
		const auto destination = _state.next_path(id == commands::browse_next_folder);

		if (!destination.empty())
		{
			hover.elements->add(std::make_shared<text_element>(destination));
		}
	}
	else if (id == commands::browse_back)
	{
		if (_state.history.can_browse_back())
		{
			hover.elements->add(std::make_shared<text_element>(_state.history.back_entry().search.text()));
		}
	}
	else if (id == commands::browse_forward)
	{
		if (_state.history.can_browse_forward())
		{
			hover.elements->add(std::make_shared<text_element>(_state.history.forward_entry().search.text()));
		}
	}
	else if (id == commands::browse_parent)
	{
		const auto parent = _state.parent_search().parent;

		if (!parent.is_empty())
		{
			hover.elements->add(std::make_shared<text_element>(parent.text(), ui::style::font_face::dialog,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::new_line));
		}
	}
	else if (id == commands::favorite)
	{
		const auto search = _state.search();

		if (!search.is_empty())
		{
			const auto is_favorite = _state.search_is_favorite();
			const auto text = str_format((is_favorite ? tt.favorite_remove_fmt : tt.favorite_add_fmt).sv(),
			                             search.text());
			hover.elements->add(std::make_shared<text_element>(text, ui::style::font_face::dialog,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::new_line));

			hover.elements->add(std::make_shared<text_element>(tt.favorite_info, ui::style::font_face::dialog,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::new_line));
		}
	}
	else if (id == commands::options_collection)
	{
		const auto search = _state.search();

		if (!search.is_empty())
		{
			const auto is_collection_root = _state.search_is_in_collection();
			const auto text = is_collection_root ? tt.collection_in : tt.collection_not_in;
			hover.elements->add(std::make_shared<text_element>(text, ui::style::font_face::dialog,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::new_line));

			hover.elements->add(std::make_shared<text_element>(tt.collection_info, ui::style::font_face::dialog,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::new_line));
		}
	}
#ifndef WINSTORE
	else if (id == commands::info_new_version)
	{
		hover.elements->add(std::make_shared<text_element>(tt.update_available, ui::style::font_face::dialog,
		                                                   ui::style::text_style::multiline,
		                                                   flex_item::line_break));
		hover.elements->add(
			std::make_shared<text_element>(str_format(tt.update_avail_version_fmt.sv(), setting.available_version)));
		hover.elements->add(
			std::make_shared<text_element>(str_format(tt.update_current_version_fmt.sv(), s_app_version)));
	}
#endif
	else if (id == commands::option_show_rotated)
	{
		const auto i = _state.command_item();

		if (i != nullptr)
		{
			const auto md = i->metadata();

			if (md)
			{
				hover.elements->add(std::make_shared<text_element>(
					str_format(tt.item_oriented_tooltip_fmt.sv(), orientation_to_string(md->orientation))));
			}
		}
	}

	if (keyboard_accelerator.empty())
	{
		if (command)
		{
			if (str::is_empty(keyboard_accelerator))
			{
				keyboard_accelerator = command->keyboard_accelerator_text;
			}
		}
	}

	if (!keyboard_accelerator.empty())
	{
		keyboard_accelerator = std::format("{} {}", tt.keyboard_accelerator_press, keyboard_accelerator);
		hover.elements->add(std::make_shared<action_element>(keyboard_accelerator));
	}
}

void app_frame::update_tooltip()
{
	_hover.clear();

	if (df::command_active == 0)
	{
		if (setting.show_help_tooltips && _app_logo_hover)
		{
			_app_logo->tooltip(_hover, {}, _app_frame->window_bounds().top_left());
		}
		else
		{
			auto c = setting.show_help_tooltips ? _view_frame->_active_controller : view_controller_ptr{};
			std::shared_ptr<view_host> frame = _view_frame;

			if (!c)
			{
				const auto& sidebar = _view_items->sidebar();
				c = sidebar->_active_controller;
				frame = sidebar;
			}

			if (c)
			{
				c->popup_from_location(_hover);

				if (_hover.id != commands::none)
				{
					tooltip(_hover, _hover.id);
				}

				const auto window_bounds = frame->frame()->window_bounds();
				_hover.window_bounds = _hover.window_bounds.offset(window_bounds.top_left());
				frame->_tooltip_bounds = _hover.active_bounds;
			}
			else if (setting.show_help_tooltips && _hover_command)
			{
				tooltip(_hover, std::any_cast<const commands>(_hover_command->opaque));
				_hover.window_bounds = _hover_command_bounds;
				_hover.active_bounds = _hover_command_bounds;
			}
		}
	}

	// The toolbars exist before init() creates the bubble, so a hover can arrive with no bubble yet.
	if (!_bubble)
	{
		return;
	}

	if (!_hover.is_empty())
	{
		_bubble->show(_hover.elements, _hover.window_bounds, _hover.x_focus, _hover.preferred_size, _hover.horizontal);
	}
	else
	{
		_bubble->hide();
	}
}
