// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Command identifier enumeration. Defines all available application commands
// used for menus, toolbars, keyboard shortcuts, and programmatic invocation.

#pragma once

#include "libdesktop/ui.h"

class view_state;
namespace ui
{
	class complete_strategy_t;
}

enum class commands
{
	none = 0,

	advanced_search,
	browse_back,
	browse_forward,
	browse_next_folder,
	browse_next_group,
	browse_next_item,
	browse_next_item_extend,
	browse_open_containingfolder,
	browse_open_googlemap,
	browse_open_in_file_browser,
	browse_parent,
	browse_previous_folder,
	browse_previous_group,
	browse_previous_item,
	browse_previous_item_extend,
	browse_recursive,
	browse_search,
	edit_copy,
	edit_copy_item_path,
	edit_cut,
	edit_item_auto_color,
	edit_item_auto_document,
	edit_item_auto_straighten,
	edit_item_preview,
	edit_item_color_reset,
	edit_item_save,
	edit_item_save_and_next,
	edit_item_save_and_prev,
	edit_item_save_as,
	edit_paste,
	english,
	exit,
	favorite,
	favorite_tags,
	filter_items,
	filter_audio,
	filter_photos,
	filter_videos,
	group_album,
	group_aspect_ratio,
	group_camera,
	group_created,
	group_original,
	group_extension,
	group_file_type,
	group_folder,
	group_location,
	group_modified,
	group_pixels,
	group_presence,
	group_rating,
	group_shuffle,
	group_size,
	group_toggle,
	import_analyze,
	import_run,
	locate_run,
#ifndef WINSTORE
	info_new_version,
	info_check_for_updates,
#endif
	keyboard,
	label_approved,
	label_none,
	label_review,
	label_second,
	label_select,
	label_to_do,
	large_font,
	menu_display_options,
	menu_zoom,
	menu_zoom_navigator,
	menu_group,
	menu_group_toolbar,
	menu_language,
	menu_main,
	menu_navigate,
	menu_open,
	menu_options,
	menu_playback,
	menu_rate_or_label,
	menu_select,
	menu_tools,
	menu_tools_toolbar,
	option_highlight_large_items,
	option_scale_up,
	option_show_rotated,
	option_toggle_details,
	option_toggle_item_size,
	options_collection,
	options_general,
	options_sidebar,
	pin_item,
	play,
	playback_auto_play,
	playback_auto_advance,
	playback_last_played_pos,
	playback_menu,
	playback_repeat_all,
	playback_repeat_none,
	playback_repeat_one,
	playback_volume_toggle,
	playback_volume0,
	playback_volume100,
	playback_volume200,
	playback_volume25,
	playback_volume50,
	playback_volume75,
	print,
	rate_1,
	rate_2,
	rate_3,
	rate_4,
	rate_5,
	rate_none,
	rate_rejected,
	refresh,
	repeat_toggle,
	search_related,
	select_all,
	select_invert,
	select_nothing,
	//show_raw_always,
	show_raw_preview,
	//show_raw_this_only,
	slideshow,
	sort_date_created,
	sort_date_original,
	sort_date_modified,
	sort_dates_ascending,
	sort_dates_descending,
	sort_def,
	sort_name,
	sort_size,
	sync_analyze,
	sync_run,
	tags_run,
	tool_adjust_date,
	tool_burn,
	tool_convert,
	tool_copy_to_folder,
	tool_delete,
	tool_desktop_background,
	tool_edit,
	tool_edit_description,
	tool_edit_metadata,
	tool_eject,
	tool_file_properties,
	tool_import,
	tool_locate,
	tool_move_to_folder,
	tool_movie,
	tool_movie_add,
	tool_movie_import,
	tool_movie_open,
	tool_movie_relink,
	tool_movie_remove,
	tool_movie_render,
	tool_movie_save,
	tool_movie_select_all,
	tool_movie_send_to_end,
	tool_new_folder,
	tool_open_with,
	// Re-plans the active task view after a run, so its results are never mistaken for a live plan.
	tool_refresh,
	tool_rename,
	tool_rotate_anticlockwise,
	tool_rotate_clockwise,
	tool_rotate_reset,
	// Runs whichever task view is active: rename, convert, metadata, or adjust date.
	tool_run,
	tool_save_current_video_frame,
	tool_scan,
	// Share where the system has a share sheet, an email draft where it does not.
	tool_share,
	tool_sync,
	tool_tag,
	verbose_metadata,
	view_cancel,
	view_close,
	view_favorite_tags,
	view_fullscreen,
	view_help,
	view_items,
	view_maximize,
	view_minimize,
	view_restore,
	view_show_sidebar,
	view_zoom,
	view_zoom_fit,
	view_zoom_fit_width,
	view_zoom_fill,
	view_zoom_toggle_fit,
	view_zoom_in,
	view_zoom_pane_flip,
	view_zoom_out,
	view_zoom_100,
	view_zoom_navigator_auto_hide,
	view_zoom_navigator_off,
};

struct command_accelerator
{
	commands id = commands::none;
	keyboard_accelerator_t key;
};

// The shipped key bindings. A table rather than a run of assignments so the set can be inspected -
// nothing else can tell whether two commands have quietly claimed the same key.
std::vector<command_accelerator> default_keyboard_accelerators();

// Configured external tools accept one {item-path}; the Open menu therefore offers them only for a
// single selected file. Association handlers keep their separate whole-selection contract.
df::file_path selected_external_tool_target(const df::item_set& selected_items);
quadd rotate_command_crop_bounds(sizei dimensions, ui::orientation stored_orientation, simple_transform transform,
                                 bool show_rotated);

using destination_folder_enumerator = std::function<df::folder_paths(const df::folder_path&)>;

std::shared_ptr<ui::complete_strategy_t> make_destination_folder_auto_complete_for_test(
	view_state& state, std::shared_ptr<void> picker_lifetime, std::shared_ptr<const df::folder_counts> folders,
	destination_folder_enumerator enumerate_child_folders = {});
