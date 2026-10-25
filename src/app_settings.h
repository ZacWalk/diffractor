// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Application settings and configuration. Defines all persistent user preferences
// including display options, collection folders, import/sync settings, and feature flags.

#pragma once

#include "model_location.h"
#include "util_interfaces.h"

enum class group_key_type : uint32_t;

constexpr auto default_custom_folder_structure = "{year}\\{created}";

// Volume boost above 100%. The device volume caps at 100%, so the 100%..200%
// setting range maps onto a 1x..media_volume_boost_gain software gain - large
// enough to lift very quiet sources well above unity.
constexpr int media_volume_boost = 2000;
constexpr double media_volume_boost_gain = 8.0;

enum class zoom_navigator_mode : uint32_t
{
	auto_hide,
	pinned,
	off
};

constexpr bool is_valid_zoom_navigator_mode(const uint32_t value) noexcept
{
	return value <= static_cast<uint32_t>(zoom_navigator_mode::off);
}

namespace features
{
	constexpr uint64_t show_photo = 1ull << 0;
	constexpr uint64_t show_video = 1ull << 1;
	constexpr uint64_t show_audio = 1ull << 3;
	constexpr uint64_t show_raw = 1ull << 5;

	constexpr uint64_t search_folder = 1ull << 8;
	constexpr uint64_t search_text = 1ull << 9;
	constexpr uint64_t search_property = 1ull << 10;
	constexpr uint64_t search_type = 1ull << 11;
	constexpr uint64_t search_flatten = 1ull << 12;
	constexpr uint64_t search_related = 1ull << 13;
	constexpr uint64_t search_duplicates = 1ull << 14;

	constexpr uint64_t edit_photo_metadata = 1ull << 16;
	constexpr uint64_t edit_photo_bitmap = 1ull << 17;
	constexpr uint64_t edit_video_metadata = 1ull << 18;
	constexpr uint64_t edit_audio_metadata = 1ull << 19;

	constexpr uint64_t tag = 1ull << 32;
	constexpr uint64_t batch_edit = 1ull << 33;
	constexpr uint64_t slideshow = 1ull << 34;
	constexpr uint64_t resize = 1ull << 35;
	constexpr uint64_t convert = 1ull << 36;
	constexpr uint64_t rotate = 1ull << 37;
	constexpr uint64_t locate = 1ull << 38;
	constexpr uint64_t email = 1ull << 39;
	constexpr uint64_t adjust_date = 1ull << 40;
	constexpr uint64_t burn_to_disk = 1ull << 41;
	constexpr uint64_t print = 1ull << 42;
	constexpr uint64_t scan = 1ull << 43;
	constexpr uint64_t import = 1ull << 44;
	// bit 45 is retired (was an unimplemented pdf tool that never recorded)
	// bit 46 is retired (was the shell Remove Metadata tool, withdrawn for rework)
	constexpr uint64_t sync = 1ull << 47;

	// One bit per view. The tool bits above only record a completed run, so they cannot
	// distinguish a view that is opened and abandoned from one that is never opened at all.
	constexpr uint64_t view_items = 1ull << 48;
	constexpr uint64_t view_media = 1ull << 49;
	constexpr uint64_t view_edit = 1ull << 50;
	constexpr uint64_t view_rename = 1ull << 51;
	constexpr uint64_t view_batch = 1ull << 52;
	constexpr uint64_t view_import = 1ull << 53;
	constexpr uint64_t view_sync = 1ull << 54;
	constexpr uint64_t view_locate = 1ull << 55;
	constexpr uint64_t view_tags = 1ull << 56;
	constexpr uint64_t view_movie = 1ull << 57;

	// The tool bits above ran out at the view bits, so later tools continue here. Email keeps its bit
	// for the draft that Share falls back to where the system has no share sheet.
	constexpr uint64_t share = 1ull << 58;

	constexpr uint64_t view_bit(const view_type v)
	{
		switch (v)
		{
		case view_type::items: return view_items;
		case view_type::media: return view_media;
		case view_type::edit: return view_edit;
		case view_type::movie: return view_movie;
		case view_type::rename: return view_rename;
		case view_type::batch: return view_batch;
		case view_type::import: return view_import;
		case view_type::sync: return view_sync;
		case view_type::locate: return view_locate;
		case view_type::tags: return view_tags;
		case view_type::none: break;
		}
		return 0;
	}
}

// Feature use accumulates from several threads: the UI thread (views, display, slideshow),
// the query worker (search terms) and file workers (burn), while the web worker reads and
// clears it after a successful report. It is therefore a single process-wide atomic rather
// than a settings_t member - settings_t is value-copied by the options dialog, which would
// otherwise silently roll the mask back to whatever it was when the dialog opened.
void record_feature_use(uint64_t f);
uint64_t features_used_since_last_report();
void load_feature_use(uint64_t f);

// Clears only the bits that were actually reported so features recorded while the report
// was in flight survive to the next one.
void clear_reported_feature_use(uint64_t reported);


// Issue #227: the default sidebar (favorite) tags must be seeded only on the very first
// run, never again. `favorite_tags` is persisted as a single string, so an empty saved
// value is otherwise indistinguishable from "never configured" and the defaults get
// re-injected on every launch, resurrecting tags the user deliberately removed. The
// persisted `favorite_tags_initialized` flag records that the user has configured
// favorites at least once; after that an empty list is respected. Existing installations
// created before the flag was added are also treated as initialized, including when their
// saved list is empty. Returns true only for a newly-created settings root with no favorites.
constexpr bool should_seed_default_favorite_tags(const bool initialized, const bool current_is_empty,
                                                 const bool settings_root_created)
{
	return !initialized && current_is_empty && settings_root_created;
}

// A start is "settled" once the window is up and the message loop has gone idle at least once.
// Anything that crashes before that repeats on every relaunch with no user action to blame it on,
// and the user has nothing to click to escape it. After this many consecutive starts that never
// settled, the next one reverts presentation to defaults and turns the graphics path off. Two,
// because one unsettled start is also what an ordinary kill or power loss during launch looks like.
constexpr uint32_t max_unsettled_starts = 2;

constexpr bool should_start_safe(const uint32_t unsettled_starts)
{
	return unsettled_starts >= max_unsettled_starts;
}

struct startup_history
{
	bool safe_start = false;
	bool record = false;
	uint32_t next_unsettled = 0;
};

// The count is shared by every Diffractor process, and launches routinely overlap - Explorer starts
// one process per selected file. Only the process holding the startup scope counts its attempt, so
// several at once cannot add up to a crash history that never happened.
constexpr startup_history decide_startup(const bool owns_startup_scope, const uint32_t unsettled_starts)
{
	if (!owns_startup_scope) return {false, false, unsettled_starts};
	return {should_start_safe(unsettled_starts), true, unsettled_starts + 1};
}


class settings_t
{
public:
	settings_t();

	void read();
	void write() const;

	// Restores every setting that decides what the window puts on screen, and forces the graphics
	// path off. Used only by a safe start, so it must not touch anything the user cannot re-reach
	// from the UI afterwards: collection roots, recent lists, copyright and task fields all stay.
	void reset_presentation();

	// Constants
	static constexpr int item_splitter_max = 10000;
	static constexpr int item_scale_count = 8;
	static constexpr int item_scale_position_max = 100;
	static int item_scale_snaps[item_scale_count];
	int item_scale_dimension() const;
	void set_item_scale_position(int position);
	void step_item_scale(int direction);

	// Width of a tool view's controls panel, as a share of the resizable width in
	// view_splitter_max units. Each view keeps its own position; zero means never dragged, so
	// the view still gets its default proportion.
	static constexpr int view_splitter_max = 10000;
	static constexpr int view_splitter_count = 7;
	std::array<int, view_splitter_count> view_splitter_positions{};
	int view_splitter(view_type view) const;
	bool set_view_splitter(view_type view, int pos);

	// A zero delay would advance the slideshow every tick and divide by zero when drawing progress.
	static constexpr int min_slideshow_delay = 1;
	static constexpr int max_slideshow_delay = 30;

	sizei thumbnail_max_dimension;
	int media_volume = 0;
	int slideshow_delay = 0;
	int item_scale = 5;
	int item_scale_position = -1;
	int item_splitter_pos = 5;
	int min_show_update_day = 0;

	int jpeg_save_quality = 0;
	int webp_quality = 0;
	bool webp_lossless = false;
	uint32_t instantiations = 0;

	std::string write_folder;
	std::string available_version;
	std::string available_test_version;
	std::string favorite_tags;
	bool favorite_tags_initialized = false;
	std::string language;
	std::string sound_device;
	std::string copyright_credit;
	bool set_copyright_credit = false;
	bool set_copyright_source = false;
	std::string copyright_source;
	bool set_copyright_creator = false;
	std::string copyright_creator;
	bool set_copyright_notice = false;
	std::string copyright_notice;
	bool set_copyright_url = false;
	std::string copyright_url;

	bool set_artist = false;
	std::string artist;
	bool set_caption = false;
	std::string caption;
	bool set_album = false;
	std::string album;
	bool set_album_artist = false;
	std::string album_artist;
	bool set_genre = false;
	std::string genre;
	bool set_tv_show = false;
	std::string tv_show;

	// Metadata task field selection. The values these apply to are per-run, but which fields are in
	// play is a working focus that should survive the view and the session.
	bool set_title = false;
	bool set_comment = false;
	bool set_synopsis = false;
	bool set_rating = false;
	bool set_year = false;
	// The stored keys are legacy: set_created has always selected the Original date, so renaming the
	// key would silently drop the field selection of every existing user.
	bool set_original = false;
	bool set_created = false;
	bool set_episode = false;
	bool set_season = false;
	bool set_track = false;
	bool set_disk = false;

	bool show_hidden = false;
	bool show_debug_info = false;
	bool confirm_deletions = false;
	// Only single-item rotations honour this; rotating several items is always confirmed.
	bool confirm_rotations = false;
	bool first_run_today = false;
	bool first_run_ever = false;
	bool show_rotated = false;
	bool show_results = false;
	bool create_originals = false;
#ifndef WINSTORE
	bool check_for_updates = false;
	bool send_crash_dumps = false;
#endif
	repeat_mode repeat = repeat_mode::repeat_none;
	zoom_navigator_mode zoom_navigator = zoom_navigator_mode::auto_hide;
	bool auto_play = false;
	bool auto_advance = false;
	bool scale_up = false;
	bool highlight_large_items = false;
	bool sort_dates_descending = false;
	bool show_sidebar = false;
	bool use_gpu = false;
	bool use_d3d11va = false;
	bool use_yuv = false;
	bool force_available_version = false;
	bool large_font = false;
	bool verbose_metadata = false;
	bool raw_preview = true;
	uint32_t detail_items = true;

	// Whether items of this media type draw as a detail list rather than thumbnails. Held as a
	// bitmask over group_key_type so the choice survives both a search that recreates the groups
	// and a restart (issue #229). Every reader and writer goes through these two accessors so the
	// bit rule cannot drift between the command, the group and the settings store.
	bool is_detail_display(group_key_type type) const;
	void set_detail_display(group_key_type type, bool detail);
	bool show_shadow = true;
	bool last_played_pos = true;
	bool show_help_tooltips = true;

	struct sidebar_t
	{
		bool show_total_items = false;
		bool show_history = false;
		bool show_world_map = false;
		bool show_drives = false;
		bool show_favorite_searches = false;
		bool show_tags = false;
		bool show_ratings = false;
		bool show_labels = false;
		bool show_favorite_tags_only = false;
		int history_start_year = 0;
		int width = 0; // user-set sidebar width in logical pixels; 0 = auto (preferred width)
	} sidebar;

	struct import_t
	{
		std::string destination_path;
		std::string source_path;
		std::string source_filter;
		std::string dest_folder_structure;
		bool is_move = false;
		bool set_created_date = false;
		bool ignore_previous = false;
		bool rename_different_attributes = false;
		collision_policy collision = collision_policy::skip;
	} import;

	struct sync_t
	{
		std::string local_path;
		std::string remote_path;
		bool sync_collection = false;
		bool sync_local_remote = false;
		bool sync_remote_local = false;
		bool sync_delete_local = false;
		bool sync_delete_remote = false;
		collision_policy collision = collision_policy::skip;
	} sync;

	struct
	{
		bool maximize = false;
	} desktop_background;

	// What Share and Email each let the user change about the files that leave the app. Each keeps
	// its own: an email draft wants small attachments, while a share usually goes somewhere that
	// wants the original.
	struct outgoing_choices_t
	{
		bool zip = false;
		bool limit = false;
		bool convert = false;
		int max_side = 0;
	};

	struct email_t : outgoing_choices_t
	{
		std::string to;
		std::string subject;
		std::string message;
	} email;

	outgoing_choices_t share;

	struct convert_t
	{
		bool to_jpeg = false;
		bool to_png = false;
		bool to_webp = false;
		int jpeg_quality = 0;
		int webp_quality = 0;
		bool webp_lossless = false;
		bool limit_dimension = false;
		int max_side = 0;
		collision_policy collision = collision_policy::block_run;
	} convert;

	struct rename_t
	{
		std::string name_template;
		std::string start_seq;
		collision_policy collision = collision_policy::block_run;
	} rename;

	struct index_t
	{
		bool pictures = false;
		bool video = false;
		bool music = false;
		bool drop_box = false;
		bool onedrive_pictures = false;
		bool onedrive_video = false;
		bool onedrive_music = false;

		std::string more_folders;
	} collection;

	struct search_t
	{
		static constexpr int count = 10;

		std::string title[count];
		std::string path[count];
	} search;

	gps_coordinate default_location;

	// Locate view: restrict the selector strip to items still missing a location.
	bool locate_only_without_location = false;
};


extern settings_t setting;

// Hands the renderer its YUV latch from setting.use_yuv, and keeps it there when a driver fault
// clears it: a backend reads ui::yuv_textures_allowed when it starts and calls ui::on_yuv_fault.
void connect_yuv_latch();

std::vector<std::string_view> split_collection_folders(std::string_view text);

// The collection roots the settings name, includes and excludes alike. Defined with the index
// workers that start from it; the Movie command line reads the same roots to search the collection.
df::index_roots index_folders(const settings_t::index_t& collection);
