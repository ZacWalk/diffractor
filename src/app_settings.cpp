// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Application settings persistence. Reads and writes all user preferences
// to the registry including display options, import/sync settings, and collection configuration.

#include "pch.h"

#include "model_items.h"

static constexpr auto s_dest_path = "dest_path";
static constexpr auto s_move = "move";
static constexpr auto s_import = "import";
static constexpr auto s_limit = "limit";
static constexpr auto s_jpg = "jpg";
static constexpr auto s_png = "png";
static constexpr auto s_webp = "webp";
static constexpr auto s_limit_dimension = "limit_dimension";
static constexpr auto s_max = "max";
static constexpr auto s_start = "start";
static constexpr auto s_album = "album";
static constexpr auto s_maximize = "maximize";
static constexpr auto s_favorite_tags_old = "FavoriteTags";
static constexpr auto s_favorite_tags = "favorite_tags";
static constexpr auto s_favorite_tags_initialized = "favorite_tags_initialized";
static constexpr auto s_out_folder = "out_folder";
static constexpr auto s_volume = "volume";
static constexpr auto s_create_originals = "create_originals";
static constexpr auto s_confirm_rotate = "confirm_rotate";
static constexpr auto s_show_rotated = "show_rotated";
static constexpr auto s_show_results = "show_results";
static constexpr auto s_check_for_updates = "check_for_updates";
static constexpr auto s_last_run = "last_run";
static constexpr auto s_pictures = "pictures";
static constexpr auto s_index = "index";
static constexpr auto s_favorite_search = "favorite_search";
static constexpr auto s_video = "video";
static constexpr auto s_music = "music";
static constexpr auto s_drop_box = "dropbox";
static constexpr auto s_title = "title";
static constexpr auto s_path = "path";
static constexpr auto s_more = "more";
static constexpr auto s_convert = "convert";
static constexpr auto s_available_version = "available_version";
static constexpr auto s_available_test_version = "available_test_version";
static constexpr auto s_is_sponsor = "awesome";
static constexpr auto s_first_time = "first";
static constexpr auto s_quality = "quality";
static constexpr auto s_webp_quality = "webp_quality";
static constexpr auto s_webp_lossless = "webp_lossless";
static constexpr auto s_slideshow_delay = "slideshow_delay";
static constexpr auto s_copyright = "copyright";
static constexpr auto s_creator = "creator";
static constexpr auto s_album_artist = "album_artist";
static constexpr auto s_genre = "genre";
static constexpr auto s_tv_show = "tv_show";
static constexpr auto s_to = "to";
static constexpr auto s_subject = "subject";
static constexpr auto s_message = "message";
static constexpr auto s_items_scale = "items_scale";
static constexpr auto s_items_scale_position = "items_scale_position";
static constexpr auto s_item_splitter = "item_splitter";
static constexpr auto s_view_splitters = "view_splitters";
static constexpr auto s_auto_play = "auto_play";
static constexpr auto s_auto_advance = "auto_advance";
static constexpr auto s_zoom_navigator = "zoom_navigator";
static constexpr auto s_scale_up = "scale_up";
static constexpr auto s_features = "features";
static constexpr auto s_update_min = "update_min";
static constexpr auto s_instantiations = "instantiations";
static constexpr auto s_highlight_large_items = "highlight_large_items";
static constexpr auto s_sort_dates_descending = "sort_dates_descending";
static constexpr auto g_show_navigation_bar = "show_nav_bar";
static constexpr auto g_detail_items = "detail_items";
static constexpr auto s_large_font = "large_font";
static constexpr auto s_use_gpu = "use_gpu";
static constexpr auto s_use_d3d11_va = "use_d3d11va";
static constexpr auto s_use_yuv = "use_yuv";
static constexpr auto s_send_crash_dumps = "send_crash_dumps";
static constexpr auto s_show_performance_timings = "show_performance_timings";
static constexpr auto s_source = "source";
static constexpr auto s_credit = "credit";
static constexpr auto s_artist = "artist";
static constexpr auto s_caption = "caption";
static constexpr auto s_url = "url";
static constexpr auto s_set_url = "set_url";
static constexpr auto s_set_copyright = "set_copyright";
static constexpr auto s_set_creator = "set_creator";
static constexpr auto s_set_source = "set_source";
static constexpr auto s_set_credit = "set_credit";
static constexpr auto s_set_artist = "set_artist";
static constexpr auto s_set_caption = "set_caption";
static constexpr auto s_set_album = "set_album";
static constexpr auto s_set_album_artist = "set_album_artist";
static constexpr auto s_set_genre = "set_genre";
static constexpr auto s_set_tv_show = "set_tv_show";
static constexpr auto s_set_title = "set_title";
static constexpr auto s_set_comment = "set_comment";
static constexpr auto s_set_synopsis = "set_synopsis";
static constexpr auto s_set_rating = "set_rating";
static constexpr auto s_set_year = "set_year";
static constexpr auto s_set_created = "set_created";
static constexpr auto s_set_date_created = "set_date_created";
static constexpr auto s_set_episode = "set_episode";
static constexpr auto s_set_season = "set_season";
static constexpr auto s_set_track = "set_track";
static constexpr auto s_set_disk = "set_disk";
static constexpr auto s_ignore_previous = "ignore_previous";
static constexpr auto s_hidden = "hidden";
static constexpr auto s_email = "email";
static constexpr auto s_share = "share";
static constexpr auto s_rename = "rename";
static constexpr auto s_collision = "collision_policy";
static constexpr auto s_template = "template";
static constexpr auto s_wall_paper = "wallpaper";
static constexpr auto s_confirm = "confirm";
static constexpr auto s_repeat = "repeat";
static constexpr auto s_zip = "zip";
static constexpr auto s_sidebar = "sidebar";
static constexpr auto s_show_total_items = "show_total_items";
static constexpr auto s_show_history = "show_history";
static constexpr auto s_show_world_map = "show_world_map";
static constexpr auto s_show_drives = "show_drives";
static constexpr auto s_show_favorite_searches = "show_favorite_searches";
static constexpr auto s_show_tags = "show_tags";
static constexpr auto s_show_ratings = "show_ratings";
static constexpr auto s_show_labels = "show_labels";
static constexpr auto s_history_years = "history_years";
static constexpr auto s_history_start_year = "history_start_year";
static constexpr auto s_sidebar_width = "width";
static constexpr auto s_lang = "lang";
static constexpr auto s_verbose_metadata = "verbose_metadata";
static constexpr auto s_location_latitude = "location_latitude";
static constexpr auto s_location_longitude = "location_longitude";
static constexpr auto s_locate_only_without_location = "locate_only_without_location";
static constexpr auto s_raw_preview = "raw_preview";
static constexpr auto s_onedrive_pictures = "onedrive_pictures";
static constexpr auto s_onedrive_video = "onedrive_video";
static constexpr auto s_onedrive_music = "onedrive_music";
static constexpr auto s_set_created_date = "set_created_date";
static constexpr auto s_sync = "sync";
static constexpr auto s_local_path = "local_path";
static constexpr auto s_remote_path = "remote_path";
static constexpr auto s_sync_collection = "sync_collection";
static constexpr auto s_sync_local_remote = "sync_local_remote";
static constexpr auto s_sync_remote_local = "sync_remote_local";
static constexpr auto s_sync_delete_local = "sync_delete_local";
static constexpr auto s_sync_delete_remote = "sync_delete_remote";
static constexpr auto s_custom_folder_structure = "dest_folder_structure";
static constexpr auto s_rename_different_attributes = "rename_different_attributes";
static constexpr auto s_source_path = "source_path";
static constexpr auto s_source_filter = "source_filter";
static constexpr auto s_show_shadow = "show_shadow";
static constexpr auto s_last_played_pos = "last_played_pos";
static constexpr auto s_show_help_tooltips = "show_help_tooltips";
static constexpr auto s_sound_device = "sound_device";

settings_t setting;

void connect_yuv_latch()
{
	ui::yuv_textures_allowed = setting.use_yuv;
	ui::on_yuv_fault = []
	{
		setting.use_yuv = false;
		setting.write();
	};
}

static std::atomic<uint64_t> s_feature_use{0};

void record_feature_use(const uint64_t f)
{
	s_feature_use.fetch_or(f, std::memory_order_relaxed);
}

uint64_t features_used_since_last_report()
{
	return s_feature_use.load(std::memory_order_relaxed);
}

void load_feature_use(const uint64_t f)
{
	s_feature_use.store(f, std::memory_order_relaxed);
}

void clear_reported_feature_use(const uint64_t reported)
{
	s_feature_use.fetch_and(~reported, std::memory_order_relaxed);
}

constexpr uint32_t operator|(group_key_type lhs, group_key_type rhs)
{
	return static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs);
}

constexpr uint32_t operator|(const uint32_t lhs, group_key_type rhs)
{
	return lhs | static_cast<uint32_t>(rhs);
}

// items that are details by default
constexpr uint32_t default_detail_items = group_key_type::folder |
	group_key_type::audio |
	group_key_type::grouped_no_value |
	group_key_type::archive |
	group_key_type::other |
	group_key_type::retro;

bool settings_t::is_detail_display(const group_key_type type) const
{
	return (detail_items & static_cast<uint32_t>(type)) != 0;
}

void settings_t::set_detail_display(const group_key_type type, const bool detail)
{
	if (detail)
	{
		detail_items |= static_cast<uint32_t>(type);
	}
	else
	{
		detail_items &= ~static_cast<uint32_t>(type);
	}
}

int settings_t::item_scale_snaps[item_scale_count];

static void init_item_scale_snaps()
{
	auto prime = 5059.0;

	for (auto i = 0; i < settings_t::item_scale_count; i++)
	{
		settings_t::item_scale_snaps[i] = df::round(std::sqrt(prime));
		prime *= 1.3333;
	}
}

static int item_scale_snap_position(const int index)
{
	const auto clamped_index = std::clamp(index, 0, settings_t::item_scale_count - 1);
	const auto min_dimension = settings_t::item_scale_snaps[0];
	const auto dimension_range = settings_t::item_scale_snaps[settings_t::item_scale_count - 1] - min_dimension;
	return df::round(static_cast<double>(settings_t::item_scale_snaps[clamped_index] - min_dimension) *
		settings_t::item_scale_position_max / std::max(1, dimension_range));
}

int settings_t::item_scale_dimension() const
{
	const auto min_dimension = item_scale_snaps[0];
	const auto dimension_range = item_scale_snaps[item_scale_count - 1] - min_dimension;
	const auto position = item_scale_position < 0 ? item_scale_snap_position(item_scale) : item_scale_position;
	return min_dimension + df::round(static_cast<double>(std::clamp(position, 0, item_scale_position_max)) *
		dimension_range / item_scale_position_max);
}

void settings_t::set_item_scale_position(const int position)
{
	item_scale_position = std::clamp(position, 0, item_scale_position_max);
	item_scale = 0;
	for (auto index = 1; index < item_scale_count; ++index)
	{
		if (std::abs(item_scale_snap_position(index) - item_scale_position) <
			std::abs(item_scale_snap_position(item_scale) - item_scale_position))
		{
			item_scale = index;
		}
	}
}

void settings_t::step_item_scale(const int direction)
{
	if (direction == 0) return;
	const auto current = item_scale_position < 0 ? item_scale_snap_position(item_scale) : item_scale_position;
	if (direction > 0)
	{
		for (auto index = 0; index < item_scale_count; ++index)
		{
			const auto position = item_scale_snap_position(index);
			if (position > current)
			{
				item_scale = index;
				item_scale_position = position;
				return;
			}
		}
		item_scale = 0;
	}
	else
	{
		for (auto index = item_scale_count - 1; index >= 0; --index)
		{
			const auto position = item_scale_snap_position(index);
			if (position < current)
			{
				item_scale = index;
				item_scale_position = position;
				return;
			}
		}
		item_scale = item_scale_count - 1;
	}

	item_scale_position = item_scale_snap_position(item_scale);
}

// Slot for each view that shows a controls panel. Order must match s_view_splitter_names so a
// stored position can never be attributed to a different view after a reorder.
static int view_splitter_index(const view_type view)
{
	switch (view)
	{
	case view_type::edit: return 0;
	case view_type::rename: return 1;
	case view_type::batch: return 2;
	case view_type::import: return 3;
	case view_type::sync: return 4;
	case view_type::locate: return 5;
	case view_type::tags: return 6;
	default: return -1;
	}
}

static constexpr std::string_view s_view_splitter_names[settings_t::view_splitter_count] = {
	"edit", "rename", "batch", "import", "sync", "locate", "tags"
};

int settings_t::view_splitter(const view_type view) const
{
	const auto index = view_splitter_index(view);
	return index < 0 ? 0 : view_splitter_positions[index];
}

bool settings_t::set_view_splitter(const view_type view, const int pos)
{
	const auto index = view_splitter_index(view);
	if (index < 0 || view_splitter_positions[index] == pos) return false;
	view_splitter_positions[index] = pos;
	return true;
}

settings_t::settings_t()
{
	init_item_scale_snaps();

	language = "en";
	show_rotated = true;
	show_results = true;
	create_originals = true;
#ifndef WINSTORE
	check_for_updates = true;
	send_crash_dumps = true;
#endif
	show_hidden = false;
	show_debug_info = false;
	use_gpu = true;
	use_d3d11va = true;
	use_yuv = true;
	confirm_deletions = true;
	confirm_rotations = true;
	first_run_today = true;
	first_run_ever = true;
	repeat = repeat_mode::repeat_none;
	auto_play = true;
	auto_advance = true;
	scale_up = false;
	highlight_large_items = true;
	sort_dates_descending = true;
	show_sidebar = true;
	large_font = false;
	verbose_metadata = true;
	raw_preview = true;
	default_location = gps_coordinate(51.5255317687988, -0.116743430495262); // London
	detail_items = default_detail_items;

	instantiations = 0;

	copyright_notice = std::format("\xC2\xA9 {} {} - All Rights Reserved",
	                               platform::now().year(),
	                               platform::user_name());
	copyright_creator = platform::user_name();
	artist = platform::user_name();

	thumbnail_max_dimension = {320, 256};
	media_volume = 1000;
	jpeg_save_quality = 90;
	webp_quality = 70;
	webp_lossless = false;
	slideshow_delay = 3;
	item_splitter_pos = item_splitter_max / 2;

	write_folder = known_path(platform::known_folder::pictures).text();

	import.source_path = known_path(platform::known_folder::onedrive_camera_roll).text();
	import.destination_path = known_path(platform::known_folder::pictures).text();
	import.dest_folder_structure = default_custom_folder_structure;
	import.is_move = false;
	import.set_created_date = true;
	import.rename_different_attributes = true;

	sync.local_path = known_path(platform::known_folder::pictures).text();
	sync.remote_path = known_path(platform::known_folder::onedrive_pictures).combine("backup").text();
	sync.sync_collection = true;
	sync.sync_local_remote = true;
	sync.sync_remote_local = false;
	sync.sync_delete_local = false;
	sync.sync_delete_remote = false;


	desktop_background.maximize = true;

	email.zip = false;
	email.convert = true;
	email.limit = true;
	email.max_side = 1024;

	// Nothing changed unless asked; the size is what a ticked limit starts from.
	share.zip = false;
	share.convert = false;
	share.limit = false;
	share.max_side = 1024;

	convert.to_jpeg = true;
	convert.to_png = false;
	convert.to_webp = false;
	convert.limit_dimension = false;
	convert.max_side = 1024;
	convert.jpeg_quality = 90;
	convert.webp_quality = 70;
	convert.webp_lossless = false;

	sidebar.show_total_items = true;
	sidebar.show_history = true;
	sidebar.show_world_map = true;
	sidebar.show_drives = true;
	sidebar.show_favorite_searches = true;
	sidebar.show_tags = true;
	sidebar.show_ratings = true;
	sidebar.show_labels = true;
	sidebar.history_start_year = 0;

	collection.pictures = true;
	collection.video = true;
	collection.music = false;
	collection.drop_box = false;
	collection.onedrive_pictures = false;
	collection.onedrive_video = false;
	collection.onedrive_music = false;

	search.title[0] = "Last 7 days";
	search.path[0] = "age: 7";
	search.title[1] = "Christmas";
	search.path[1] = "created: dec.25";
	search.title[2] = "Desktop";
	search.path[2] = known_path(platform::known_folder::desktop).text();
	search.title[3] = "Downloads";
	search.path[3] = known_path(platform::known_folder::downloads).text();
	search.title[4] = "Pictures";
	search.path[4] = known_path(platform::known_folder::pictures).text();
	search.title[5] = "Videos";
	search.path[5] = known_path(platform::known_folder::video).text();

	rename.name_template = "Item ###";
	rename.start_seq = "1";

	set_copyright_credit = false;
	set_copyright_source = false;
	set_copyright_creator = false;
	set_copyright_notice = false;
	set_copyright_url = false;
	set_artist = false;
	set_caption = false;
	set_album = false;
	set_album_artist = false;
	set_genre = false;
	set_tv_show = false;
	set_title = false;
	set_comment = false;
	set_synopsis = false;
	set_rating = false;
	set_year = false;
	set_original = false;
	set_created = false;
	set_episode = false;
	set_season = false;
	set_track = false;
	set_disk = false;

	available_version = s_app_version;
};

void settings_t::reset_presentation()
{
	// Taken from a fresh instance rather than repeated literals, so this cannot drift away from
	// what a first run actually looks like.
	const settings_t defaults;

	show_sidebar = defaults.show_sidebar;
	sidebar = defaults.sidebar;
	large_font = defaults.large_font;
	show_debug_info = defaults.show_debug_info;
	show_shadow = defaults.show_shadow;
	zoom_navigator = defaults.zoom_navigator;
	item_scale = defaults.item_scale;
	item_scale_position = defaults.item_scale_position;
	item_splitter_pos = defaults.item_splitter_pos;
	view_splitter_positions = defaults.view_splitter_positions;
	detail_items = defaults.detail_items;

	// Not the defaults: a crash before the first frame is most often the graphics path, and the
	// user cannot turn it off from a window that never appears. use_yuv is left alone: it is a
	// latch only a driver fault clears and no option sets again, so clearing it here disabled YUV
	// textures for good. Turning the GPU off already keeps them out of this session.
	use_gpu = false;
	use_d3d11va = false;
}

class setting_formatter
{
	platform::setting_file_ptr _file;

public:
	setting_formatter(const platform::setting_file_ptr& f) : _file(f)
	{
	}

	bool write(const std::string_view section, const std::string_view name, const uint32_t v) const
	{
		return _file->write(section, name, v);
	}

	bool read(const std::string_view section, const std::string_view name, uint32_t& v) const
	{
		return _file->read(section, name, v);
	}

	bool write(const std::string_view section, const std::string_view name, const uint64_t v) const
	{
		return _file->write(section, name, v);
	}

	bool read(const std::string_view section, const std::string_view name, uint64_t& v) const
	{
		return _file->read(section, name, v);
	}

	bool write(const std::string_view section, const std::string_view name, const int v) const
	{
		return _file->write(section, name, std::bit_cast<uint32_t>(v));
	}

	bool read(const std::string_view section, const std::string_view name, int& v) const
	{
		uint32_t vv{};
		if (_file->read(section, name, vv))
		{
			v = vv;
			return true;
		}
		return false;
	}

#if !DF_LONG_IS_INT64
	bool write(const std::string_view section, const std::string_view name, const long v) const
	{
		return _file->write(section, name, std::bit_cast<uint32_t>(v));
	}
#endif

	bool read(const std::string_view section, const std::string_view name, int64_t& v) const
	{
		uint64_t vv{};
		if (_file->read(section, name, vv))
		{
			v = vv;
			return true;
		}
		return false;
	}

	bool read(const std::string_view section, const std::string_view name, repeat_mode& v) const
	{
		// Reject values outside the closed ontology rather than inventing a state: an enumerator no
		// switch has an arm for behaves as none while the menu ticks nothing.
		uint32_t vv{};
		if (!_file->read(section, name, vv) || !is_valid_repeat_mode(vv)) return false;
		v = static_cast<repeat_mode>(vv);
		return true;
	}

	bool read(const std::string_view section, const std::string_view name, zoom_navigator_mode& v) const
	{
		uint32_t value{};
		if (!_file->read(section, name, value) || !is_valid_zoom_navigator_mode(value)) return false;
		v = static_cast<zoom_navigator_mode>(value);
		if (v == zoom_navigator_mode::pinned) v = zoom_navigator_mode::auto_hide;
		return true;
	}

	bool write(const std::string_view section, const std::string_view name, const zoom_navigator_mode v) const
	{
		return _file->write(section, name, static_cast<uint32_t>(v));
	}

	bool read(const std::string_view section, const std::string_view name, collision_policy& v) const
	{
		uint32_t vv{};
		if (_file->read(section, name, vv))
		{
			// Reject values outside the closed ontology rather than inventing a state.
			if (vv > static_cast<uint32_t>(collision_policy::auto_rename)) return false;
			v = static_cast<collision_policy>(vv);
			return true;
		}
		return false;
	}

	bool write(const std::string_view section, const std::string_view name, const collision_policy v) const
	{
		return _file->write(section, name, static_cast<uint32_t>(v));
	}

	bool write(const std::string_view section, const std::string_view name, const int64_t v) const
	{
		return _file->write(section, name, std::bit_cast<uint64_t>(v));
	}

	bool read(const std::string_view section, const std::string_view name, double& v) const
	{
		std::string str;

		// Only a value that was really there may replace the default. Converting unconditionally
		// wrote to_double("") - which is zero - over whatever the constructor had set, so the first
		// read of a setting that had never been written silently zeroed it.
		if (!_file->read(section, name, str)) return false;

		v = str::to_double(str);
		return true;
	}

	bool write(const std::string_view section, const std::string_view name, const double v) const
	{
		return _file->write(section, name, str::to_string(v, 5));
	}

#if !DF_LONG_IS_INT64
	bool read(const std::string_view section, const std::string_view name, long& v) const
	{
		uint32_t vv{};
		if (_file->read(section, name, vv))
		{
			v = vv;
			return true;
		}
		return false;
	}
#endif

	bool write(const std::string_view section, const std::string_view name, const bool v) const
	{
		const uint32_t dw = v ? 1 : 0;
		return _file->write(section, name, dw);
	}

	bool read(const std::string_view section, const std::string_view name, bool& v) const
	{
		uint32_t dw = 0;

		if (!_file->read(section, name, dw))
			return false;

		v = dw != 0;
		return true;
	}

	bool read(const std::string_view section, const std::string_view name, std::string& str) const
	{
		return _file->read(section, name, str);
	}

	bool write(const std::string_view section, const std::string_view name, const std::string_view v) const
	{
		return _file->write(section, name, v);
	}

	bool read(const std::string_view section, const std::string_view name, std::string* v, const int count) const
	{
		auto success = true;

		for (auto i = 0; i < count; i++)
		{
			success &= read(section, std::format("{}{}", name, i), v[i]);
		}

		return success;
	}

	bool write(const std::string_view section, const std::string_view name, const std::string* v, const int count)
	{
		auto success = true;

		for (auto i = 0; i < count; i++)
		{
			success &= write(section, std::format("{}{}", name, i), v[i]);
		}

		return success;
	}

	bool read(const std::string_view section, const std::string_view name, std::vector<uint8_t>& v) const
	{
		constexpr size_t buffer_size = 1024 * 8;
		uint8_t buffer[buffer_size];
		size_t n = buffer_size;
		const auto success = read(section, name, buffer, n);

		if (success)
		{
			v.assign(buffer, buffer + n);
		}

		return success;
	}

	bool write(const std::string_view section, const std::string_view name, const std::vector<uint8_t>& v) const
	{
		return write(section, name, {v.data(), v.size()});
	}

	bool read(const std::string_view section, const std::string_view name,
	          df::hash_map<std::string, std::string, df::ihash, df::ieq>& v) const
	{
		std::string combined;
		const auto success = read(section, name, combined);

		if (success)
		{
			const auto parts = str::split(combined, true, str::is_white_space);

			for (const auto& part : parts)
			{
				const auto split = part.find(':');

				if (split != std::string_view::npos)
				{
					v[std::string(part.substr(0, split))] = part.substr(split + 1);
				}
			}
		}

		return success;
	}

	bool write(const std::string_view section, const std::string_view name,
	           const df::hash_map<std::string, std::string, df::ihash, df::ieq>& v)
	{
		std::string combined;
		for (const auto& s : v) str::join(combined, s.first + ":"s + s.second);
		return write(section, name, combined);
	}

	bool read(const std::string_view section, const std::string_view name, uint8_t* pValue, size_t& n) const
	{
		return _file->read(section, name, pValue, n);
	}

	bool write(const std::string_view section, const std::string_view name, const df::cspan cs) const
	{
		return _file->write(section, name, cs);
	}
};

void settings_t::read()
{
	const setting_formatter store(platform::settings());

	store.read({}, s_hidden, show_hidden);
	store.read({}, s_show_shadow, show_shadow);
	store.read({}, s_last_played_pos, last_played_pos);
	store.read({}, s_show_help_tooltips, show_help_tooltips);
	store.read({}, s_show_performance_timings, show_debug_info);
	store.read({}, s_use_gpu, use_gpu);
	store.read({}, s_use_d3d11_va, use_d3d11va);
	store.read({}, s_use_yuv, use_yuv);
	store.read({}, s_confirm, confirm_deletions);
	store.read({}, s_confirm_rotate, confirm_rotations);
	store.read({}, s_repeat, repeat);
	store.read({}, s_zoom_navigator, zoom_navigator);
	store.read({}, s_auto_play, auto_play);
	store.read({}, s_auto_advance, auto_advance);
	store.read({}, s_scale_up, scale_up);
	store.read({}, s_highlight_large_items, highlight_large_items);
	store.read({}, s_sort_dates_descending, sort_dates_descending);
	store.read({}, g_show_navigation_bar, show_sidebar);
	store.read({}, g_detail_items, detail_items);
	store.read({}, s_volume, media_volume);
	store.read({}, s_quality, jpeg_save_quality);
	store.read({}, s_webp_quality, webp_quality);
	store.read({}, s_webp_lossless, webp_lossless);
	store.read({}, s_slideshow_delay, slideshow_delay);
	slideshow_delay = std::clamp(slideshow_delay, min_slideshow_delay, max_slideshow_delay);
	store.read({}, s_items_scale, item_scale);
	store.read({}, s_items_scale_position, item_scale_position);
	if (item_scale_position < 0) item_scale_position = item_scale_snap_position(item_scale);
	set_item_scale_position(item_scale_position);
	store.read({}, s_item_splitter, item_splitter_pos);

	for (auto i = 0; i < view_splitter_count; ++i)
	{
		store.read(s_view_splitters, s_view_splitter_names[i], view_splitter_positions[i]);
	}

	store.read({}, s_update_min, min_show_update_day);
	store.read({}, s_large_font, large_font);
	store.read({}, s_verbose_metadata, verbose_metadata);
	store.read({}, s_raw_preview, raw_preview);
	store.read({}, s_lang, language);
	store.read({}, s_sound_device, sound_device);
	store.read({}, s_out_folder, write_folder);
	store.read({}, s_show_rotated, show_rotated);
	store.read({}, s_show_results, show_results);
	store.read({}, s_create_originals, create_originals);
#ifndef WINSTORE
	store.read({}, s_check_for_updates, check_for_updates);
	store.read({}, s_send_crash_dumps, send_crash_dumps);
#endif

	auto lat = default_location.latitude();
	auto lng = default_location.longitude();
	store.read({}, s_location_latitude, lat);
	store.read({}, s_location_longitude, lng);
	default_location.latitude(lat);
	default_location.longitude(lng);

	store.read({}, s_locate_only_without_location, locate_only_without_location);

	store.read({}, s_available_version, available_version);
	store.read({}, s_available_test_version, available_test_version);
	store.read({}, s_favorite_tags_old, favorite_tags);
	store.read({}, s_favorite_tags, favorite_tags);
	store.read({}, s_favorite_tags_initialized, favorite_tags_initialized);

	store.read({}, s_copyright, copyright_notice);
	store.read({}, s_creator, copyright_creator);
	store.read({}, s_source, copyright_source);
	store.read({}, s_credit, copyright_credit);
	store.read({}, s_url, copyright_url);
	store.read({}, s_artist, artist);
	store.read({}, s_caption, caption);
	store.read({}, s_album, album);
	store.read({}, s_album_artist, album_artist);
	store.read({}, s_genre, genre);
	store.read({}, s_tv_show, tv_show);

	store.read({}, s_set_copyright, set_copyright_credit);
	store.read({}, s_set_creator, set_copyright_creator);
	store.read({}, s_set_source, set_copyright_source);
	store.read({}, s_set_credit, set_copyright_notice);
	store.read({}, s_set_url, set_copyright_url);
	store.read({}, s_set_artist, set_artist);
	store.read({}, s_set_caption, set_caption);
	store.read({}, s_set_album, set_album);
	store.read({}, s_set_album_artist, set_album_artist);
	store.read({}, s_set_genre, set_genre);
	store.read({}, s_set_tv_show, set_tv_show);
	store.read({}, s_set_title, set_title);
	store.read({}, s_set_comment, set_comment);
	store.read({}, s_set_synopsis, set_synopsis);
	store.read({}, s_set_rating, set_rating);
	store.read({}, s_set_year, set_year);
	store.read({}, s_set_created, set_original);
	store.read({}, s_set_date_created, set_created);
	store.read({}, s_set_episode, set_episode);
	store.read({}, s_set_season, set_season);
	store.read({}, s_set_track, set_track);
	store.read({}, s_set_disk, set_disk);

	uint64_t stored_feature_use = 0;
	store.read({}, s_features, stored_feature_use);
	load_feature_use(stored_feature_use);
	store.read({}, s_instantiations, instantiations);

	int last_run = 0;
	store.read({}, s_last_run, last_run);
	first_run_today = last_run != static_cast<int>(platform::now().to_days());
	store.read({}, s_first_time, first_run_ever);

	store.read(s_rename, s_template, rename.name_template);
	store.read(s_rename, s_start, rename.start_seq);
	store.read(s_rename, s_collision, rename.collision);

	store.read(s_import, s_source_path, import.source_path);
	store.read(s_import, s_source_filter, import.source_filter);
	store.read(s_import, s_dest_path, import.destination_path);
	store.read(s_import, s_move, import.is_move);
	store.read(s_import, s_set_created_date, import.set_created_date);
	store.read(s_import, s_ignore_previous, import.ignore_previous);
	store.read(s_import, s_custom_folder_structure, import.dest_folder_structure);
	store.read(s_import, s_rename_different_attributes, import.rename_different_attributes);
	store.read(s_import, s_collision, import.collision);

	store.read(s_sync, s_local_path, sync.local_path);
	store.read(s_sync, s_remote_path, sync.remote_path);
	store.read(s_sync, s_sync_collection, sync.sync_collection);
	store.read(s_sync, s_sync_local_remote, sync.sync_local_remote);
	store.read(s_sync, s_sync_remote_local, sync.sync_remote_local);
	store.read(s_sync, s_sync_delete_local, sync.sync_delete_local);
	store.read(s_sync, s_sync_delete_remote, sync.sync_delete_remote);
	store.read(s_sync, s_collision, sync.collision);

	store.read(s_wall_paper, s_maximize, desktop_background.maximize);

	store.read(s_convert, s_jpg, convert.to_jpeg);
	store.read(s_convert, s_png, convert.to_png);
	store.read(s_convert, s_webp, convert.to_webp);
	store.read(s_convert, s_limit_dimension, convert.limit_dimension);
	store.read(s_convert, s_max, convert.max_side);
	store.read(s_convert, s_quality, convert.jpeg_quality);
	store.read(s_convert, s_webp_quality, convert.webp_quality);
	store.read(s_convert, s_webp_lossless, convert.webp_lossless);
	store.read(s_convert, s_collision, convert.collision);

	store.read(s_email, s_to, email.to);
	store.read(s_email, s_subject, email.subject);
	store.read(s_email, s_message, email.message);
	store.read(s_email, s_zip, email.zip);
	store.read(s_email, s_convert, email.convert);
	store.read(s_email, s_limit, email.limit);
	store.read(s_email, s_max, email.max_side);

	store.read(s_share, s_zip, share.zip);
	store.read(s_share, s_convert, share.convert);
	store.read(s_share, s_limit, share.limit);
	store.read(s_share, s_max, share.max_side);

	store.read(s_index, s_pictures, collection.pictures);
	store.read(s_index, s_video, collection.video);
	store.read(s_index, s_music, collection.music);
	store.read(s_index, s_drop_box, collection.drop_box);
	store.read(s_index, s_onedrive_pictures, collection.onedrive_pictures);
	store.read(s_index, s_onedrive_video, collection.onedrive_video);
	store.read(s_index, s_onedrive_music, collection.onedrive_music);
	store.read(s_index, s_more, collection.more_folders);


	store.read(s_favorite_search, s_title, search.title, search.count);
	store.read(s_favorite_search, s_path, search.path, search.count);

	store.read(s_sidebar, s_show_total_items, sidebar.show_total_items);
	store.read(s_sidebar, s_show_history, sidebar.show_history);
	store.read(s_sidebar, s_show_world_map, sidebar.show_world_map);
	store.read(s_sidebar, s_show_drives, sidebar.show_drives);
	store.read(s_sidebar, s_show_favorite_searches, sidebar.show_favorite_searches);
	store.read(s_sidebar, s_show_tags, sidebar.show_tags);
	store.read(s_sidebar, s_show_ratings, sidebar.show_ratings);
	store.read(s_sidebar, s_show_labels, sidebar.show_labels);
	store.read(s_sidebar, s_favorite_tags, sidebar.show_favorite_tags_only);
	if (!store.read(s_sidebar, s_history_start_year, sidebar.history_start_year))
	{
		int history_years = 0;
		if (store.read(s_sidebar, s_history_years, history_years) && history_years > 0)
		{
			sidebar.history_start_year = platform::now().date().year - history_years + 1;
		}
	}
	store.read(s_sidebar, s_sidebar_width, sidebar.width);
}

void settings_t::write() const
{
	setting_formatter store(platform::settings());

	store.write({}, s_hidden, show_hidden);
	store.write({}, s_show_shadow, show_shadow);
	store.write({}, s_last_played_pos, last_played_pos);
	store.write({}, s_show_help_tooltips, show_help_tooltips);
	store.write({}, s_show_performance_timings, show_debug_info);
	store.write({}, s_use_gpu, use_gpu);
	store.write({}, s_use_d3d11_va, use_d3d11va);
	store.write({}, s_use_yuv, use_yuv);
	store.write({}, s_confirm, confirm_deletions);
	store.write({}, s_confirm_rotate, confirm_rotations);
	store.write({}, s_repeat, static_cast<int>(repeat));
	store.write({}, s_zoom_navigator, zoom_navigator);
	store.write({}, s_auto_play, auto_play);
	store.write({}, s_auto_advance, auto_advance);
	store.write({}, s_scale_up, scale_up);
	store.write({}, s_highlight_large_items, highlight_large_items);
	store.write({}, s_sort_dates_descending, sort_dates_descending);
	store.write({}, g_show_navigation_bar, show_sidebar);
	store.write({}, g_detail_items, detail_items);
	store.write({}, s_volume, media_volume);
	store.write({}, s_quality, jpeg_save_quality);
	store.write({}, s_webp_quality, webp_quality);
	store.write({}, s_webp_lossless, webp_lossless);
	store.write({}, s_slideshow_delay, slideshow_delay);
	store.write({}, s_items_scale, item_scale);
	store.write({}, s_items_scale_position, item_scale_position);
	store.write({}, s_item_splitter, item_splitter_pos);

	for (auto i = 0; i < view_splitter_count; ++i)
	{
		store.write(s_view_splitters, s_view_splitter_names[i], view_splitter_positions[i]);
	}

	store.write({}, s_update_min, min_show_update_day);
	store.write({}, s_large_font, large_font);
	store.write({}, s_verbose_metadata, verbose_metadata);
	store.write({}, s_raw_preview, raw_preview);
	store.write({}, s_lang, language);
	store.write({}, s_sound_device, sound_device);
	store.write({}, s_out_folder, write_folder);
	store.write({}, s_show_rotated, show_rotated);
	store.write({}, s_show_results, show_results);
	store.write({}, s_create_originals, create_originals);
#ifndef WINSTORE
	store.write({}, s_check_for_updates, check_for_updates);
	store.write({}, s_send_crash_dumps, send_crash_dumps);
#endif
	store.write({}, s_last_run, platform::now().to_days());
	store.write({}, s_location_latitude, default_location.latitude());
	store.write({}, s_location_longitude, default_location.longitude());
	store.write({}, s_locate_only_without_location, locate_only_without_location);
	store.write({}, s_available_version, available_version);
	store.write({}, s_available_test_version, available_test_version);
	store.write({}, s_favorite_tags, favorite_tags);
	store.write({}, s_favorite_tags_initialized, favorite_tags_initialized);
	store.write({}, s_first_time, first_run_ever);
	store.write({}, s_copyright, copyright_notice);
	store.write({}, s_creator, copyright_creator);
	store.write({}, s_source, copyright_source);
	store.write({}, s_credit, copyright_credit);
	store.write({}, s_url, copyright_url);
	store.write({}, s_artist, artist);
	store.write({}, s_caption, caption);
	store.write({}, s_album, album);
	store.write({}, s_album_artist, album_artist);
	store.write({}, s_genre, genre);
	store.write({}, s_tv_show, tv_show);

	store.write({}, s_set_copyright, set_copyright_credit);
	store.write({}, s_set_creator, set_copyright_creator);
	store.write({}, s_set_source, set_copyright_source);
	store.write({}, s_set_credit, set_copyright_notice);
	store.write({}, s_set_url, set_copyright_url);
	store.write({}, s_set_artist, set_artist);
	store.write({}, s_set_caption, set_caption);
	store.write({}, s_set_album, set_album);
	store.write({}, s_set_album_artist, set_album_artist);
	store.write({}, s_set_genre, set_genre);
	store.write({}, s_set_tv_show, set_tv_show);
	store.write({}, s_set_title, set_title);
	store.write({}, s_set_comment, set_comment);
	store.write({}, s_set_synopsis, set_synopsis);
	store.write({}, s_set_rating, set_rating);
	store.write({}, s_set_year, set_year);
	store.write({}, s_set_created, set_original);
	store.write({}, s_set_date_created, set_created);
	store.write({}, s_set_episode, set_episode);
	store.write({}, s_set_season, set_season);
	store.write({}, s_set_track, set_track);
	store.write({}, s_set_disk, set_disk);

	store.write({}, s_features, features_used_since_last_report());
	store.write({}, s_instantiations, instantiations);

	store.write(s_rename, s_template, rename.name_template);
	store.write(s_rename, s_start, rename.start_seq);
	store.write(s_rename, s_collision, rename.collision);

	store.write(s_import, s_source_path, import.source_path);
	store.write(s_import, s_source_filter, import.source_filter);
	store.write(s_import, s_dest_path, import.destination_path);
	store.write(s_import, s_move, import.is_move);
	store.write(s_import, s_set_created_date, import.set_created_date);
	store.write(s_import, s_ignore_previous, import.ignore_previous);
	store.write(s_import, s_custom_folder_structure, import.dest_folder_structure);
	store.write(s_import, s_rename_different_attributes, import.rename_different_attributes);
	store.write(s_import, s_collision, import.collision);

	store.write(s_sync, s_local_path, sync.local_path);
	store.write(s_sync, s_remote_path, sync.remote_path);
	store.write(s_sync, s_sync_collection, sync.sync_collection);
	store.write(s_sync, s_sync_local_remote, sync.sync_local_remote);
	store.write(s_sync, s_sync_remote_local, sync.sync_remote_local);
	store.write(s_sync, s_sync_delete_local, sync.sync_delete_local);
	store.write(s_sync, s_sync_delete_remote, sync.sync_delete_remote);
	store.write(s_sync, s_collision, sync.collision);

	store.write(s_wall_paper, s_maximize, desktop_background.maximize);

	store.write(s_convert, s_jpg, convert.to_jpeg);
	store.write(s_convert, s_png, convert.to_png);
	store.write(s_convert, s_webp, convert.to_webp);
	store.write(s_convert, s_limit_dimension, convert.limit_dimension);
	store.write(s_convert, s_max, convert.max_side);
	store.write(s_convert, s_quality, convert.jpeg_quality);
	store.write(s_convert, s_webp_quality, convert.webp_quality);
	store.write(s_convert, s_webp_lossless, convert.webp_lossless);
	store.write(s_convert, s_collision, convert.collision);

	store.write(s_email, s_to, email.to);
	store.write(s_email, s_subject, email.subject);
	store.write(s_email, s_message, email.message);
	store.write(s_email, s_zip, email.zip);
	store.write(s_email, s_convert, email.convert);
	store.write(s_email, s_limit, email.limit);
	store.write(s_email, s_max, email.max_side);

	store.write(s_share, s_zip, share.zip);
	store.write(s_share, s_convert, share.convert);
	store.write(s_share, s_limit, share.limit);
	store.write(s_share, s_max, share.max_side);

	store.write(s_index, s_pictures, collection.pictures);
	store.write(s_index, s_video, collection.video);
	store.write(s_index, s_music, collection.music);
	store.write(s_index, s_drop_box, collection.drop_box);
	store.write(s_index, s_onedrive_pictures, collection.onedrive_pictures);
	store.write(s_index, s_onedrive_video, collection.onedrive_video);
	store.write(s_index, s_onedrive_music, collection.onedrive_music);
	store.write(s_index, s_more, collection.more_folders);


	store.write(s_favorite_search, s_title, search.title, search.count);
	store.write(s_favorite_search, s_path, search.path, search.count);

	store.write(s_sidebar, s_show_total_items, sidebar.show_total_items);
	store.write(s_sidebar, s_show_history, sidebar.show_history);
	store.write(s_sidebar, s_show_world_map, sidebar.show_world_map);
	store.write(s_sidebar, s_show_drives, sidebar.show_drives);
	store.write(s_sidebar, s_show_favorite_searches, sidebar.show_favorite_searches);
	store.write(s_sidebar, s_show_tags, sidebar.show_tags);
	store.write(s_sidebar, s_show_ratings, sidebar.show_ratings);
	store.write(s_sidebar, s_show_labels, sidebar.show_labels);
	store.write(s_sidebar, s_favorite_tags, sidebar.show_favorite_tags_only);
	store.write(s_sidebar, s_history_start_year, sidebar.history_start_year);
	store.write(s_sidebar, s_sidebar_width, sidebar.width);
}

std::vector<std::string_view> split_collection_folders(const std::string_view text)
{
	return str::split(text, true, [](const char c) { return c == '\n' || c == '\r'; });
}
