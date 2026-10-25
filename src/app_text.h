// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Localized text strings and translations. Contains all user-visible text for
// internationalization support with PO file loading and plural text formatting.

#pragma once

struct av_stream_info;

std::string language_name(std::string_view code);
std::string format_audio_stream_name(const av_stream_info& stream, int audio_track_number);


using text_mapping = df::hash_map<std::string_view, std::reference_wrapper<text_t>>;

struct plural_text
{
	plural_text(const std::string_view o, const std::string_view p) : one(o), plural(p)
	{
	}

	text_t one;
	text_t plural;

	// Translated plural forms with index >= 2 (e.g. the Slavic "many" form), from msgstr[2..].
	// English and binary-plural languages leave it null; one/plural cover forms 0 and 1. Published
	// the way a text_t is - a pointer to storage app_text_t never frees - because a worker formatting
	// a count can hold a form across a language switch, which reassigned a vector under it.
	std::atomic<const std::vector<std::string>*> extra_forms{nullptr};

	std::string_view extra_form(const size_t index) const
	{
		const auto* const forms = extra_forms.load(std::memory_order_acquire);
		return forms != nullptr && index < forms->size() ? std::string_view((*forms)[index]) : std::string_view{};
	}
};

struct po_entry
{
	std::string id;
	std::string str;
	std::string id_plural;
	std::string str_plural;
	// Extra translated plural forms from msgstr[2], msgstr[3], ... in order.
	// Only Slavic languages declare these; str is msgstr[0], str_plural is msgstr[1].
	std::vector<std::string> str_extra;

	bool is_empty() const
	{
		return id.empty() &&
			str_plural.empty() &&
			str.empty() &&
			id_plural.empty() &&
			str_extra.empty();
	}

	void clear()
	{
		id.clear();
		str_plural.clear();
		str.clear();
		id_plural.clear();
		str_extra.clear();
	}
};

constexpr size_t max_supported_plural_forms = 4;
constexpr size_t max_supported_plural_index = max_supported_plural_forms - 1;

struct po_parse_error
{
	size_t line = 0;
	std::string message;
};

struct po_load_result
{
	std::vector<po_entry> entries;
	std::vector<po_parse_error> errors;
};

po_load_result load_po_report(df::file_path lang_file);
std::vector<po_entry> load_po(df::file_path lang_file);

std::string format_plural_text(const plural_text& fmt, int64_t count, int64_t of_total = 0);
std::string format_plural_text(const plural_text& fmt, std::string_view first_name, int64_t count,
                               df::file_size size, int64_t of_total = 0);
std::string format_plural_text(const plural_text& fmt, const df::item_set& items);
std::string format_plural_text(const plural_text& fmt, const std::vector<std::string>& result);

std::string_view tt_prep(std::string_view);

// Which media a known genre name belongs to, so a picker can offer only the genres that
// make sense for the current selection.
enum class genre_kind : uint32_t
{
	none = 0,
	audio = 1 << 0,
	video = 1 << 1,
	photo = 1 << 2,
	any = audio | video | photo,
};

constexpr genre_kind operator|(const genre_kind a, const genre_kind b)
{
	return static_cast<genre_kind>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

constexpr genre_kind& operator|=(genre_kind& a, const genre_kind b)
{
	a = a | b;
	return a;
}

constexpr bool operator&&(const genre_kind a, const genre_kind b)
{
	return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

struct app_text_t
{
	text_mapping _text_mapping;
	std::vector<std::reference_wrapper<plural_text>> _all_plurals;
	std::vector<std::reference_wrapper<text_t>> _all_texts;

	// Every translation this session has loaded. A switch publishes new strings and leaves the
	// previous ones in place, because a worker that read a translation pointer just before the
	// switch still has to find valid storage behind it. A deque, so appending never moves what is
	// already published. Bounded by the catalog size times the number of switches in one session.
	std::deque<std::string> _translation_storage;

	// The same for the extra plural forms of each plural text, which are published as a set.
	std::deque<std::vector<std::string>> _plural_form_storage;

	// Plural rule for the active language. Maps a count to a form index.
	// Defaults to the binary one/plural rule used by English.
	int (*_plural_rule)(int64_t) = nullptr;

	app_text_t();

	app_text_t(const app_text_t& other1) = delete;
	app_text_t(app_text_t&& other1) noexcept = delete;
	app_text_t& operator=(const app_text_t& other1) = delete;
	app_text_t& operator=(app_text_t&& other1) noexcept = delete;

	void load_lang(std::string_view lang_file, const std::vector<po_entry>& entries);

	// Selects the plural form index (0-based) for count in the active language.
	// Non-Slavic languages use a binary one/plural rule (index 0 or 1); Slavic
	// languages add "few"/"many" forms. See app_text.cpp for the rule table.
	int plural_form(int64_t count) const;

	void clear();
	void calc_text_mapping();

	std::string translate_text(const std::string& text, std::string_view scope = {}) const;
	std::vector<std::string> add_translate_text(const std::vector<str::cached>& text,
	                                            std::string_view scope = {}) const;
	// Translated, sorted names of the built-in genres that apply to kinds.
	std::vector<std::string> known_genres(genre_kind kinds) const;

	text_t nav_search_title = "Search";
	text_t nav_history_title = "History";
	text_t error_invalid_path_fmt = "The file name you have chosen '{}' is invalid.";
	text_t error_invalid_path = "File names cannot contain the following characters:";
	text_t error_create_file_failed_fmt = "The destination file '{}' cannot be opened or created.";
	text_t error_rename_failed = "Failed to rename.";
	text_t error_create_folder_failed_fmt = "The destination folder '{}' cannot be opened or created.";
	text_t error_create_window_failed = "Diffractor cannot be started. The main window could not be created.";
	text_t error_index_database_failed = "Index database failed.";
	text_t command_menu_group_sort = "Group or Sort";
	text_t command_view_sort = "Sort";
	text_t command_view_menu = "Menu";
	text_t tooltip_view_menu = "More options and controls";
	text_t command_rotate_reset = "Reset rotation";
	text_t tooltip_rotate_reset = "Reset straighten and rotate";
	text_t command_color_reset = "Reset color changes";
	text_t tooltip_color_reset = "Reset color adjustments";
	text_t command_view_select = "Select Items";
	text_t command_view_rate_label = "Rate or Label";
	text_t command_navigate = "Navigate";
	text_t command_display_options = "Display options";
	text_t command_view_help = "About Diffractor";
	text_t command_browse_parent = "Parent";
	text_t command_browse_back = "Back";
	text_t command_browse_forward = "Forward";
	text_t command_new_folder = "New Folder";
	text_t command_rename = "Rename";
	text_t command_rename_files = "Rename files";
	text_t command_locate = "Add location";
	text_t command_print = "Print";
	text_t command_nav_bar = "Show sidebar";
	text_t tooltip_nav_bar = "Contains common folders, searches and favorites";
	text_t command_delete = "Delete";
	text_t command_select_invert = "Invert selection";
	text_t command_copy = "Copy to folder";
	text_t command_move = "Move to folder";
	text_t command_file_properties = "Open properties";
	text_t command_share_email = "Email";
	text_t email_preparing = "Preparing files for email...";
	text_t command_share = "Share";
	text_t share_preparing = "Preparing files to share...";
	text_t command_desktop_background = "Desktop background";
	text_t command_select_nothing = "Select none";
	text_t command_options = "General options";
	text_t command_customise = "Customise sidebar";
	text_t command_collection_options = "Collection options";
	text_t command_view_items = "Show items";
	text_t command_flatten = "Show items in subfolders";
	text_t command_refresh = "Refresh";
	text_t command_fullscreen = "Fullscreen";
	text_t tooltip_fullscreen = "Toggle full screen for media-items or open non-media-items.";
	text_t command_autoplay = "Autoplay selected media";
	text_t command_auto_advance = "Continue with the next item";
	text_t auto_advance_help = "When an item finishes, continue with the next photo, video or audio item";
	text_t command_last_played_pos = "Resume playing media from last position";
	text_t command_repeat_toggle = "Toggle repeat play";
	text_t command_playback_menu = "Playback Options";
	text_t command_playback_toolbar = "Playback";
	text_t audio_track_current_fmt = "Audio: {}";
	text_t audio_track_fmt = "Audio track {}";
	text_t audio_tracks_title = "Audio tracks";
	text_t audio_output_title = "Audio output";
	text_t audio_role_commentary = "commentary";
	text_t audio_role_description = "audio description";
	text_t command_repeat_one = "Repeat play one item";
	text_t command_repeat_all = "Repeat play all media items";
	text_t command_repeat_none = "Don't repeat";
	text_t command_scale_up = "Scale up items";
	text_t tooltip_scale_up =
		"Off: photos and videos smaller than the window are shown at their real size. On: they are enlarged to fit the window. Media larger than the window is always scaled down to fit.";
	text_t command_select_all = "Select all";
	text_t command_related = "Related items";
	text_t tooltip_related =
		"Show the items in the collection most closely related to this one: possible copies, then the same album, the same series, the closest capture times, and the closest capture places.";
	text_t related_group_duplicates = "This item and possible copies";
	text_t related_group_album = "Same album";
	text_t related_group_series = "Same series";
	text_t related_group_time = "Taken at the same time";
	text_t related_group_location = "Taken in the same place";
	text_t command_pin = "Pin or hold item";
	text_t tooltip_pin =
		"Pinned items stay selected. Use this when you want to quickly compare an item to other items.";
	text_t command_toggle_item_size = "Toggle thumbnail size";
	text_t tooltip_thumbnail_size = "Change thumbnail size";
	text_t tooltip_scroll_to_top = "Back to the top of the list";
	text_t command_convert_or_resize = "Convert or Resize";
	text_t command_convert = "Convert";
	text_t convert_preparing = "Preparing to convert files...";
	text_t command_edit_metadata = "Edit metadata";
	text_t command_update_metadata = "Update metadata";
	text_t command_burn = "Burn to disk";
	text_t command_keyboard = "Keyboard reference";
	text_t command_open_with = "Open with";
	text_t command_open = "Open";
	text_t tooltip_open = "Open selected items with external app";
	text_t command_language = "Language";
	text_t tooltip_tag_with = "Tag or un-tag selected items";
	text_t tooltip_language = "Change language";
	text_t command_tools = "Tools";
	text_t tooltip_tools = "Process selected items";
	text_t command_eject = "Eject";
	text_t command_minimize = "Minimize";
	text_t command_maximize = "Maximize";
	text_t command_restore = "Restore";
	text_t command_close = "Close";
	text_t command_cancel = "Cancel";
	text_t command_import = "Import";
	text_t import_preparing = "Preparing to import files...";
	text_t command_browse_previous_group = "Previous item group";
	text_t command_browse_next_group = "Next item group";
	text_t command_browse_previous_folder = "Previous folder or search";
	text_t command_browse_next_folder = "Next folder or search";
	text_t command_browse_previous_item = "Select previous item";
	text_t command_browse_previous_item_extend = "Extend selection to previous item";
	text_t command_browse_next_item = "Select next item";
	text_t command_browse_next_item_extend = "Extend selection to next item";
	text_t command_play = "Play/Pause";
	text_t tooltip_play = "Play or pause the displayed video or audio file.";
	text_t command_slideshow = "Slideshow";
	text_t tooltip_slideshow =
		"Play through the visible items. Photos are shown for the slideshow delay; video and audio play to the end.";
	text_t command_zoom = "Zoom mode";
	text_t command_zoom_fit = "Fit";
	text_t command_zoom_fit_width = "Fit Width";
	text_t command_zoom_fill = "Fill";
	text_t command_zoom_presets = "Zoom";
	text_t command_zoom_in = "Zoom in";
	text_t command_zoom_flip = "Flip comparison panes";
	text_t command_zoom_out = "Zoom out";
	text_t command_zoom_100 = "100% (1:1)";
	text_t command_zoom_navigator = "Navigator";
	text_t command_zoom_navigator_auto_hide = "Auto-hide";
	text_t command_zoom_navigator_off = "Off";
	text_t command_toggle_volume = "Toggle volume";
	text_t command_volume200 = "Volume 200%";
	text_t command_volume100 = "Volume 100%";
	text_t command_volume75 = "Volume 75%";
	text_t command_volume50 = "Volume 50%";
	text_t command_volume25 = "Volume 25%";
	text_t command_volume0 = "Mute";
	text_t command_rotate_anticlockwise = "Rotate anticlockwise";
	text_t command_rotate_clockwise = "Rotate clockwise";
	text_t rotate_confirm_single = "Confirm single-item rotations";
	text_t command_show_in_folder = "Open containing folder";
	text_t command_show_in_file_browser = "Show in file browser";
	text_t command_file_search = "Search";
	text_t command_toggle_details = "Toggle thumbnails or details";
	text_t tooltip_toggle_details_selected = "Toggle between thumbnails or details for selected item group";
	text_t tooltip_toggle_details_all = "Toggle between thumbnails or details view";
	text_t command_capture = "Save current video frame";
	text_t command_edit = "Edit";
	text_t tooltip_edit1 = "Color, straighten or crop photos.";
	text_t command_edit_copy = "Copy";
	text_t command_edit_copy_item_path = "Copy item path";
	text_t command_edit_cut = "Cut";
	text_t command_edit_paste = "Paste";
	text_t command_app_exit = "Exit";
	text_t command_rate_1 = "Rate as 1 star";
	text_t command_rate_2 = "Rate as 2 stars";
	text_t command_rate_3 = "Rate as 3 stars";
	text_t command_rate_4 = "Rate as 4 stars";
	text_t command_rate_5 = "Rate as 5 stars";
	text_t command_rate_0 = "Remove rating";
	text_t command_rate_rejected = "Reject";
	text_t command_label_approved = "Approved";
	text_t command_label_to_do = "To Do";
	text_t command_label_select = "Select";
	text_t command_label_review = "Review";
	text_t command_label_second = "Second";
	text_t command_label_none = "No Label";
	text_t command_group_shuffle = "Show items in random order";
	text_t command_toggle_group_by = "Toggle item grouping";
	text_t command_group_file_type = "Group by File type";
	text_t command_group_size = "Group by Size";
	text_t command_group_extension = "Group by Extension";
	text_t command_group_location = "Group by Location";
	text_t command_group_rating = "Group by Rating/Label";
	text_t command_group_created = "Group by Date Created";
	text_t command_group_original = "Group by Date Original";
	text_t command_group_modified = "Group by Date Modified";
	text_t command_group_resolution = "Group by Resolution";
	text_t command_group_camera = "Group by Camera";
	text_t command_group_album = "Group by Album/Show";
	text_t command_group_aspect_ratio = "Group by Aspect ratio";
	text_t command_group_presence = "Group by Collection presence";
	text_t command_group_folder = "Group by Folder";
	text_t command_sort_dates_descending = "Show newest items first";
	text_t command_sort_dates_ascending = "Show oldest items first";
	text_t command_sort_name = "Sort by Name";
	text_t command_sort_size = "Sort by Size";
	text_t command_sort_def = "Sort by Group order (Default)";
	text_t command_sort_date_created = "Sort by Date created";
	text_t command_sort_date_original = "Sort by Date original";
	text_t command_sort_date_modified = "Sort by Date modified";
	text_t command_highlight_large_items = "Highlight large items in yellow";
	text_t command_open_google_map = "Open in google maps";
	text_t command_adjust_date = "Adjust date and time";
	text_t command_view_large_font = "Large font";
	text_t command_scan = "Scan";
	text_t command_save = "Save";
	text_t command_save_as = "Save as";
	text_t command_save_and_back = "Save and open previous";
	text_t command_save_and_next = "Save and open next";
	text_t command_locate_and_back = "Add location and open previous";
	text_t command_locate_and_next = "Add location and open next";
	text_t command_auto_color = "Auto color";
	text_t tooltip_auto_color = "Automatically improve exposure and color balance";
	text_t command_auto_document = "Auto document";
	text_t tooltip_auto_document = "Detect and straighten a photographed document";
	text_t command_auto_straighten = "Auto straighten";
	text_t tooltip_auto_straighten = "Automatically straighten horizontal and vertical lines";
	text_t edit_original = "Original";
	text_t command_edit_preview = "Preview";
	text_t tooltip_edit_preview = "Show the final cropped image";
	text_t command_new_version = "New version available";
	text_t command_check_for_updates = "Check for updates";
	text_t searching_text = "Searching...";
	text_t disk_label = "Label";
	text_t disk_capacity = "Capacity";
	text_t disk_free = "Free//disk";
	text_t disk_used = "Used";
	text_t disk_system = "System";
	text_t duplicates = "Duplicates";
	text_t duplicates_tooltip = "Matching or possible copy items in the collection.";
	text_t unknown = "Unknown";
	text_t sort_by_presence = "Presence";
	text_t sort_by_file_type = "Type";
	text_t sort_by_shuffle = "Show items in random order";
	text_t sort_by_name = "Name";
	text_t sort_by_size = "Size";
	text_t sort_by_def = "Group order (Default)";
	text_t sort_by_extension = "Extension";
	text_t sort_by_location = "Location";
	text_t sort_by_album_show = "Album-Show";
	text_t sort_by_rating_label = "Rating-Label";
	text_t sort_by_resolution = "Resolution";
	text_t sort_by_aspect_ratio = "Aspect ratio";
	text_t sort_by_Folder = "Folder";
	text_t no_items_are_selected = "No items are selected";
	text_t not_supported_cloud = "Cloud items are not supported.";
	text_t not_supported_readonly = "Readonly items are not supported.";
	text_t not_supported_readonly_metadata = "Cannot update metadata of readonly items.";
	text_t not_supported_photo = "Items must be a photo or image.";
	text_t not_supported_photo_edit =
		"Select one photo to edit. Videos, audio, folders and formats Diffractor cannot save are not supported.";
	text_t not_supported_save_format = "Diffractor can only save or update JPG, WEBP and PNG image files.";
	text_t cannot_edit_fmt =
		"Diffractor cannot write ratings, labels or other metadata into {} files, and does not write a sidecar for them.";
	text_t not_supported_folder = "Folders are not supported.";
	text_t rating_remove_fmt = "Click {} to remove rating";
	text_t rating_keys = "Number keys 0-5";
	text_t rating_replaces_reject = "Replaces Reject";
	text_t label_click_apply = "Click to apply";
	text_t label_click_clear = "Click to clear";
	text_t label_replaces_fmt = "Replaces the current label: {}";
	text_t label_reject_replaces_rating = "Replaces the star rating";
	text_t show_related = "press R to view the items most closely related to this one";
	text_t item_oriented = "Show items rotated";
	text_t show_verbose_metadata = "Show verbose metadata";
	text_t hide_verbose_metadata = "Hide verbose metadata";
	text_t item_oriented_tooltip_fmt = "This item is shown oriented based on camera information. [{}]";
	text_t num_of = "of";
	text_t before = "Before";
	text_t after = "After";
	text_t search_or_folder = "Search criteria or folder";
	text_t button_change = "Change...";
	text_t button_ok = "&OK";
	text_t button_cancel = "&Cancel";
	text_t button_close = "&Close";
	text_t version = "Version";
	text_t orientation_top_left = "top-left";
	text_t orientation_top_right = "top-right";
	text_t orientation_bottom_right = "bottom-right";
	text_t orientation_bottom_left = "bottom-left";
	text_t orientation_left_top = "left-top";
	text_t orientation_right_top = "right-top";
	text_t orientation_right_bottom = "right-bottom";
	text_t orientation_left_bottom = "left-bottom";
	text_t default_favorite_tags =
		"family food friends landscape nature night portrait selfie todo travel urban viewed";
	text_t title_rate = "rate";
	text_t title_folder = "folder:{}";
	text_t type_to_search = "Type to search";
	text_t prop_name_album = "Album";
	text_t prop_name_show = "Show";
	text_t prop_name_season = "Season";
	text_t prop_name_episode = "Episode";
	text_t prop_name_artist = "Artist";
	text_t prop_name_albumartist = "Album artist";
	text_t prop_name_audiocodec = "Audio codec";
	text_t prop_name_bitrate = "Bitrate";
	text_t prop_name_cameramanufacturer = "Camera manufacturer";
	text_t prop_name_camera = "Camera";
	text_t prop_name_channels = "Channels";
	text_t prop_name_samplerate = "Sample rate";
	text_t prop_name_sampletype = "Sample type";
	text_t prop_name_place = "Place";
	text_t prop_name_comment = "Comment";
	text_t prop_name_description = "Description";
	text_t prop_name_composer = "Composer";
	text_t prop_name_copyrightcredit = "Copyright credit";
	text_t prop_name_copyrightsource = "Copyright source";
	text_t prop_name_copyrightcreator = "Copyright creator";
	text_t prop_name_copyrightnotice = "Copyright notice";
	text_t prop_name_copyrighturl = "Copyright URL";
	text_t prop_name_country = "Country//property";
	text_t prop_name_createdexif = "Created exif";
	text_t prop_name_original = "Original//date";
	text_t prop_name_digitized = "Digitized";
	text_t prop_name_created = "Created";
	text_t prop_name_disk = "Disk";
	text_t prop_name_dimensions = "Dimensions";
	text_t prop_name_duration = "Duration";
	text_t prop_name_encoder = "Encoder";
	text_t prop_name_encodingtool = "Encoding tool";
	text_t prop_name_exposure = "Exposure";
	text_t prop_name_fnumber = "FNumber";
	text_t prop_name_focallength = "Focal length";
	text_t prop_name_35mmequivalent = "Focal length 35mm equivalent";
	text_t prop_name_pixelformat = "Pixel format";
	text_t prop_name_genre = "Genre";
	text_t prop_name_iso = "ISO";
	text_t prop_name_altitude = "Altitude";
	text_t prop_name_gps_speed = "GPS speed";
	text_t height_in_flight = "In flight";
	text_t height_altitude_fmt = "{} m";
	text_t height_underwater_fmt = "{} m underwater";
	text_t distance_label_fmt = "Distance: {}";
	text_t tooltip_search_distance = "Change how far from this place to search";
	// The eight compass abbreviations, north first and clockwise, comma separated. Translate the
	// abbreviations, keep the order and the commas.
	text_t compass_points = "N,NE,E,SE,S,SW,W,NW";
	// Distance, compass point, then place: "410 km NW of Lisbon, Portugal".
	text_t bearing_fmt = "{} {} of {}";
	text_t location_near_fmt = "Near {}";
	text_t location_remote = "Remote area";
	text_t nothing_within_fmt = "Nothing within {} of {}.";
	text_t nothing_at_place_fmt = "Nothing found at {}.";
	text_t widen_to_fmt = "Widen to {}";
	text_t search_all_named_fmt = "Search all places named {}";
	text_t show_without_location = "Show items with no location";
	text_t search_within_fmt = "Search within {} of {}";
	text_t height_high_altitude = "High altitude";
	text_t timeline_title = "Timeline";
	text_t places_title = "Places";
	// Start of the visit, then the end: "From 12 June 2019 to 21 June 2019".
	text_t visit_range_fmt = "From {} to {}";
	text_t visit_items_fmt = "{} items";
	text_t grouped_by_fmt = "Grouped by {}";
	// Grouping, then the sort order within each group: "Grouped by location - by date".
	text_t grouped_and_sorted_fmt = "Grouped by {} - {}";
	text_t clear_timeline_selection = "Clear";
	text_t prop_name_latitude = "Latitude";
	text_t prop_name_lens = "Lens";
	text_t prop_name_longitude = "Longitude";
	text_t prop_name_mediacategory = "Media category";
	text_t prop_name_megapixels = "Megapixels";
	text_t prop_name_modified = "Modified";
	text_t prop_name_null = "Null";
	text_t prop_name_orientation = "Orientation";
	text_t prop_name_publisher = "Publisher";
	text_t prop_name_performer = "Performer";
	text_t prop_name_rating = "Rating";
	text_t prop_name_size = "Size";
	text_t prop_name_state = "State";
	text_t prop_name_streams = "Streams";
	text_t prop_name_synopsis = "Synopsis";
	text_t prop_name_tag = "Tag";
	text_t prop_name_title = "Title";
	text_t prop_name_track = "Track";
	text_t prop_name_videocodec = "Video codec";
	text_t prop_name_year = "Year";
	text_t prop_name_id = "Id";
	text_t prop_name_filename = "Filename";
	text_t prop_name_rawfile = "Raw file";
	text_t prop_name_system = "System";
	text_t prop_name_game = "Game//property";
	text_t prop_name_label = "Label";
	text_t prop_name_doc_id = "Document Id";
	text_t menu_move = "Move";
	text_t error_analysis_cancelled = "The analysis was canceled.";
	text_t indexing = "Indexing";
	text_t indexing_message =
		"Diffractor is reading your collection so search, duplicates and totals stay complete. You can keep browsing while it works.";
	text_t empty = "Empty";
	text_t loading = "Loading...";
	text_t image_too_large = "Too large to display";
	text_t image_display_failed = "This image could not be displayed";
	text_t nothing_found1 = "Nothing found.";
	text_t nothing_found2 = "Search only covers folders in your collection.";
	text_t nothing_found_add_folders = "Add folders to your collection (Ctrl+F6)";
	text_t empty_folder = "Empty Folder";
	text_t button_save = "&Save";
	text_t button_dont_save = "&Don't Save";
	text_t save_changes = "Save Changes";
	text_t save_as_jpeg_fmt = "Diffractor cannot crop, rotate or color {} files. You can save the file as a JPEG?";
	text_t button_save_as_jpeg = "As &JPEG";
	text_t changes = "Changes";
	text_t has_changes = "{} has modifications";
	text_t saving_file_name = "Saving {}...";
	text_t options_backup_copy = "When overwriting a photo with irreversible changes make an original backup copy";
	text_t options_jpeg_quality = "JPEG save quality";
	text_t options_webp_quality = "WEBP save quality";
	text_t help_tag1 = "Separate each tag with a space: Australia holiday camping.";
	text_t help_tag2 = "Join 2 words together in one tag using quotes: \"national park\".";
	text_t help_artist = "Separate each artist with a comma ',' or slash '/'";
	text_t straighten = "Straighten";
	text_t perspective_horizontal = "Horizontal perspective";
	text_t perspective_vertical = "Vertical perspective";
	text_t color = "Color";
	text_t temperature = "Temperature";
	text_t tint = "Tint";
	text_t vibrance = "Vibrance";
	text_t darks = "Darks";
	text_t midtones = "Midtones";
	text_t lights = "Lights";
	text_t contrast = "Contrast";
	text_t brightness = "Brightness";
	text_t saturation = "Saturation";
	text_t metadata = "Metadata";
	text_t staring = "Staring";
	text_t tags_title = "Tags";
	text_t album_artist = "Album Artist";
	text_t copyright_title = "Copyright";
	// Month names and booleans: the foundation's words, which this text translates and installs.
	str::words words;
	// The platform layer's messages, which this text translates and installs.
	platform::messages platform_text;
	text_t compare = "Compare";
	text_t compare_tooltip = "Hold down left mouse button to compare items.";
	text_t zoom_kb = "press ctrl+space";
	text_t query_or = "or";
	text_t query_and = "and";
	text_t query_with = "with";
	text_t query_without = "without";
	text_t query_created = "created";
	text_t query_original = "original";
	text_t query_modified = "modified";
	text_t query_age = "age";
	text_t query_related = "related";
	text_t query_duplicates = "duplicates";
	text_t query_duplicates_alt1 = "dups";
	text_t query_duplicates_alt2 = "duplicate";

	text_t pixels_title = "Pixels";
	text_t pixels_icon = "icon";
	text_t pixels_small = "small";
	text_t resolution_none = "No resolution";
	text_t group_title_no_extension = "No extension";
	text_t group_title_no_location = "No location";
	text_t group_title_no_rating = "No rating";
	text_t group_title_no_resolution = "No resolution";
	text_t group_title_no_aspect_ratio = "No aspect ratio";
	text_t aspect_ratio_other_landscape = "Other landscape";
	text_t aspect_ratio_other_portrait = "Other portrait";
	text_t group_title_no_camera = "No camera";
	text_t group_title_no_album_or_show = "No album or show";
	text_t group_title_no_value = "No value";
	text_t group_title_shuffle = "random";
	text_t group_title_size_range_fmt = "{} to {}";
	text_t group_title_today = "Today";
	text_t group_title_yesterday = "Yesterday";
	text_t group_title_items = "Items";
	text_t cancel_was_pressed_after = "Cancel was pressed after {}";
	text_t rate_title = "Rate selected items";
	text_t open_in_browser_title = "Open in file browser";
	text_t new_folder_name = "New Folder";
	text_t new_folder_title = "New Folder";
	text_t burn_title = "Burn";
	text_t burn_help =
		"The selected items will be staged in Windows Disc Burning. Windows will guide you through writing the disc.";
	text_t burn_stage = "&Stage files";
	text_t burn_failed = "Windows could not stage the selected items for disc burning.";
	text_t print_title = "Print";
	// The Remove Metadata tool is withdrawn for rework; the string stays so its translations survive.
	text_t remove_metadata_title = "Remove Metadata";
	text_t folder_noun = "folder";
	text_t folder_noun_plural = "folders";
	text_t rename_help_template_1 = "When specifying a template:";
	text_t rename_help_template_2 = "Use '#' to specify position of the numeric sequence";
	text_t rename_help_template_3 =
		"Use metadata templates '{property-name}' to include file properties in the name.";
	text_t for_example = "For example:";
	text_t rename_help_template_example_2 = "photo-###";
	text_t rename_help_template_example_3 = "{year}-{month}-###";
	text_t rename_help_template_example_4 = "travel-{country}-###";

	text_t rename_info =
		"Rename the selected files from a template that can combine text, an incrementing number, and metadata properties. Review the new names below before you run.";
	text_t rename_template_label = "Template:";
	text_t rename_template_start_label = "Start at:";
	text_t rename_label = "New name:";
	text_t open_properties_title = "Open Properties";
	text_t pasted_file_name = "pasted";
	text_t open_dest = "Open destination folder afterwards";
	text_t failed_to_create_folder_fmt = "Failed to create folder: {}";
	text_t button_rotate = "&Rotate";
	text_t desktop_background_info = "Set the desktop background to the current image.";
	text_t maximize_image = "Maximize image";
	text_t button_delete = "&Delete";
	text_t delete_error = "Failed to Delete Selected File.";
	text_t button_update = "&Update";
	text_t metadata_select_field =
		"Select at least one metadata field to update. Checked empty fields clear existing values; unchecked fields are unchanged.";
	text_t genre_separator_help = "Use a semicolon to list more than one genre. For example: Rock; Blues";
	text_t copyright_notice = "Notice";
	text_t copyright_creator = "Creator";
	text_t copyright_source = "Source";
	text_t copyright_credit = "Credit";
	text_t copyright_url = "URL";
	text_t location_overwrite_gps = "Overwrite GPS coordinates when setting a location.";
	text_t location_not_selected = "No location selected.";
	text_t destination_folder = "Destination folder:";
	text_t lossless_compression = "Lossless compression";
	text_t limit_output_dimensions = "Limit output to a maximum dimension (in pixels).";
	text_t dimension_must_be_positive = "Maximum dimension must be at least 1 pixel.";
	text_t button_convert = "&Convert";
	text_t tag_add_remove = "Add or remove Tags";
	text_t help_tag_add_remove = "Tags can be removed by prefixing with a minus: -remove";
	text_t tag_add_or_remove_label = "Tags to add or remove:";
	text_t tags_favorite_label = "Favorite Tags";
	text_t tags_common_label = "Common Tags";
	text_t tags_workflow_label = "Workflow Tags";
	text_t tags_remove_label = "Removable Tags";
	text_t configure_favorite_tags = "Configure favorite tags.";
	text_t button_tag = "&Tag";
	text_t command_apply_tags = "Apply tags";
	text_t tags_column_item = "Item";
	text_t tags_column_result = "Tags after run";
	text_t tags_nothing_to_do = "Enter one or more tags to add or remove.";
	text_t tags_unchanged = "no change";
	text_t adjust_date_help1 =
		"Capture metadata and the Windows file created time will be shifted to start at a new local date and time.";
	text_t adjust_date_help2 =
		"Time gaps between dated items are preserved. Items without a known date receive the new starting date.";
	text_t adjust_date_help3 =
		"This is useful if your camera date was set wrongly and you need to fix photo and video dates.";
	text_t adjust_date_required = "Choose a valid new starting date and time.";
	text_t convert_folder_required = "Choose a destination folder.";
	text_t convert_dimension_required = "Enter a maximum dimension in pixels.";
	text_t metadata_fields_required = "Check one or more fields to change.";
	text_t selected_date_range_label = "Selected items are in the date range:";
	text_t starting_fmt = "Starting {}";
	text_t ending_fmt = "Ending {}";
	text_t starting_date_label = "New Starting Date (local time):";
	text_t command_rate = "Rate";
	text_t is_not_valid_folder_fmt = "'{}' is not a valid folder.";
	text_t title_updating = "Updating...";
	text_t open_with_app_tool = "Open with app or tool";
	text_t open_with_tool = "tool";
	text_t open_with_app = "app";
	text_t import_info =
		"Copy or move files into a folder structure built from metadata such as date, album, or artist. If the destination is in your collection, the copies join it automatically. Analyze first to review every action before you run.";
	text_t import_dest_folder = "Destination root folder. Structured folders will be created under this folder.";
	text_t import_ignore_previous = "Ignore previously imported items.";
	text_t import_replace_warning =
		"The existing files will be overwritten. Their current contents cannot be recovered from the recycle bin.";
	text_t import_dest_folder_structure = "Destination folder structure:";
	text_t move_items = "Move source items.";
	text_t import_set_created_date = "Set file date created to metadata date created.";
	text_t eject_help = "To avoid losing data, eject an external hard drive or USB drive before removing it.";
	text_t eject_failed_fmt = "Failed to eject {}.";
	text_t eject_none_found = "No removable drive was found.";
	text_t close = "Close";
	text_t eject_close_info = "Close without ejecting";
	text_t eject_title_fmt = "Eject {}";
	text_t scan_failed = "Failed to Scan.";
	text_t update_title = "Updates";
	text_t update_help_fmt = "Version {} of Diffractor is available. You are currently using version {}.";
	text_t update = "Update";
	text_t update_install_now = "Install now";
	text_t update_checking = "Checking for updates...";
	text_t update_up_to_date_fmt = "You are using the latest version ({}).";
	text_t update_check_failed = "Could not check for updates. Check your internet connection and try again.";
	text_t update_help = "Download and install now. Diffractor will need to be closed briefly during the install.";
	text_t update_not_now = "Not now";
	text_t update_not_now_help = "Remind me again in a week";
	text_t update_more_info = "More info";
	text_t update_more_info_help = "Learn more about this update at Diffractor.com";
	text_t update_please_wait = "Please wait while the update is downloaded...";
	text_t update_failed = "Update failed.";
	text_t donate = "Donate";
	text_t donate_help =
		"Diffractor can be used for free. If you find it useful, please help the project by donating.";
	text_t donate_link = "Donate online.";
	text_t documentation = "Documentation";
	text_t releases = "Releases - Diffractor";
	text_t keyboard = "Keyboard";
	text_t support = "Support";
	text_t list_of_accelerators = "List of keyboard accelerators";
	text_t learn_more_diffractor_com = "Learn more at Diffractor.com";
	text_t defragment_and_compact = "Defragment database.";
	text_t reset_database = "Clean database and reindex.\nAll data is regenerated.";
	text_t keyboard_basics_title = "Basics";
	text_t keyboard_navigation_title = "Navigation";
	text_t keyboard_playback_title = "Media playback";
	text_t keyboard_tools_title = "Tools";
	text_t keyboard_open_title = "Open";
	text_t keyboard_file_management_title = "File management";
	text_t keyboard_rate_label_title = "Rate or Label";
	text_t keyboard_selection_title = "Selection";
	text_t keyboard_help_title = "Help";
	text_t keyboard_group_title = "Grouping";
	text_t keyboard_edit_title = "Editing";
	text_t options_title = "Options";
	text_t keyboard_ref_title = "Keyboard reference";
	text_t keyboard_enter = "enter";
	text_t keyboard_enter_desc = "Opens an image full screen.";
	text_t keyboard_space = "space";
	text_t keyboard_space_desc = "Plays or pauses video and audio. Stops a running slideshow.";
	text_t keyboard_escape = "escape";
	text_t keyboard_escape_desc = "Gets you out of full screen, zoom or other modes.";
	text_t keyboard_left = "left";
	text_t keyboard_right = "right";
	text_t keyboard_back = "back";
	text_t keyboard_browser_back = "browser-back";
	text_t keyboard_browser_favorites = "browser-favorites";
	text_t keyboard_browser_forward = "browser-forward";
	text_t keyboard_browser_home = "browser-home";
	text_t keyboard_browser_refresh = "browser-refresh";
	text_t keyboard_browser_search = "browser-search";
	text_t keyboard_browser_stop = "browser-stop";
	text_t keyboard_del = "del";
	text_t keyboard_down = "down";
	text_t keyboard_home = "home";
	text_t keyboard_end = "end";
	text_t keyboard_f1 = "F1";
	text_t keyboard_f10 = "F10";
	text_t keyboard_f11 = "F11";
	text_t keyboard_f2 = "F2";
	text_t keyboard_f3 = "F3";
	text_t keyboard_f4 = "F4";
	text_t keyboard_f5 = "F5";
	text_t keyboard_f6 = "F6";
	text_t keyboard_f7 = "F7";
	text_t keyboard_f8 = "F8";
	text_t keyboard_f9 = "F9";
	text_t keyboard_insert = "insert";
	text_t keyboard_media_next_track = "media-next-track";
	text_t keyboard_media_play_pause = "media-play-pause";
	text_t keyboard_media_prev_track = "media-prev-track";
	text_t keyboard_media_stop = "media-stop";
	text_t keyboard_next = "page-down";
	text_t keyboard_oem_4 = "[";
	text_t keyboard_oem_6 = "]";
	text_t keyboard_oem_plus = "+";
	text_t keyboard_oem_minus = "-";
	text_t keyboard_prior = "page-up";
	text_t keyboard_tab = "tab";
	text_t keyboard_up = "up";
	text_t keyboard_volume_down = "volume-down";
	text_t keyboard_volume_mute = "volume-mute";
	text_t keyboard_volume_up = "volume-up";
	text_t keyboard_left_right_desc = "Move to the next or previous item.";
	text_t keyboard_ctrl_left_right_desc = "Extend the current item selection and allow comparing of items.";
	text_t keyboard_up_down_desc = "Move up or down a row of items. Pans a zoomed image.";
	text_t keyboard_home_end_desc =
		"Move to the first or last item. Pans a zoomed image to the left or right edge.";
	text_t keyboard_zoom_keys_desc = "Zoom a displayed image in or out. Press 0 to fit it to the window.";
	text_t about_info =
		"Diffractor organizes photos, videos and music where they already are. You choose the folders that make up your collection; indexing them adds fast search, duplicate detection and metadata editing without moving or copying anything.";
	text_t defragmenting = "Defragmenting...";
	text_t resetting = "Cleaning...";
	text_t options_app_options = "Application options";
	text_t options_save_options = "When saving";
	text_t options_updates = "Updates";
	text_t options_advanced = "Advanced";
	text_t index_maintenance_title = "Maintenance";
	text_t options_show_rotated = "Show photos rotated based on recorded camera orientation";
	text_t options_show_hidden = "Show hidden files";
	text_t options_confirm_del = "Confirm when deleting items. Deleted items are moved to the recycle bin.";
	text_t options_confirm_rotate = "Confirm when rotating one item. Rotating several items is always confirmed.";
	text_t option_slideshow_title = "Slideshow";
	text_t option_slideshow_delay = "Delay before showing the next item (in seconds)";
	text_t options_check_for_update = "Check for updates by connecting to the internet";
	text_t options_use_gpu = "Use hardware acceleration to draw the Diffractor user interface (requires restart)";
	text_t options_use_gpu_video = "Use hardware acceleration to decode video (when available)";
	text_t options_send_crash_reports =
		"Help make Diffractor better. If Diffractor crashes send anonymous diagnostics to Diffractor HQ. No personal data is sent - just crash diagnostics.";
	text_t options_show_debug_info = "Show application debugging information (for the programmers).";
	text_t options_show_shadow = "Show an inset shadow surrounding the image view panel.";
	text_t options_last_played_pos = "Resume playing media from the last position played.";
	text_t options_show_help_tooltips = "Show tooltips with help on buttons and links.";
	text_t index_maintenance_help =
		"Defragmenting or cleaning can improve performance. The index stores a copy of metadata from your media files. It can be rebuilt from the original files.";
	text_t index_maintenance_reset_recommended =
		"The index is reporting errors. Restarting Diffractor is recommended.";
	text_t collection_options_more_folders = "One folder per line.";
	text_t collection_options_local_folders_title = "Collection folders";
	text_t collection_options_custom_folders_title = "Other folders";
	text_t collection_options_custom_folders_help =
		"Prefix an entry with '-' to exclude it. A full path such as -c:\\photos\\secret excludes one folder. A name such as -secret, or a wildcard such as -proxy*, excludes matching folders anywhere in your collection.";

	text_t collection_options_custom_locations_help =
		"Folders can be drives, folders, network shares or device labels. Labels are useful for removable or network devices where mapped drive letters can change.";
	text_t customise_tags_title = "Favorite Tags";
	text_t customise_tags_help = "Add a list of your favorite tags here to save time when tagging.";
	text_t customise_searches_title = "Favorite Searches";
	text_t customise_sidebar_title = "Sidebar";
	text_t customise_sidebar_desc = "Define what items are shown in the sidebar.";
	text_t customize_show_total = "Show total items pie chart";
	text_t customize_show_history = "Show history chart";
	text_t customize_history_start_year = "Earliest year the history navigator offers. Empty works it out from your photos.";
	text_t customize_show_world_map = "Show world map";
	text_t customize_show_drives = "Show drives";
	text_t customize_show_searches = "Show favorite searches";
	text_t customize_show_tags = "Show tags";
	text_t customize_ratings = "Show ratings";
	text_t customize_labels = "Show labels";
	text_t email_small_help =
		"Create a draft in your desktop email client with the selected files attached. Originals are unchanged.";
	text_t email_zip = "Zip files in the email";
	text_t email_convert_to_jpeg = "Convert all photos to JPEG";
	text_t email_limit_dimensions =
		"Limit photos to a maximum dimension (in pixels). Only supported for JPEG and PNG files.";
	text_t button_send = "&Send";
	text_t email_processing_fmt = "Processing {}";
	text_t email_connecting_to_mapi = "Connecting to MAPI...";
	text_t email_sending = "Sending email using MAPI...";
	text_t email_canceled = "Email draft was canceled.";
	text_t email_failed = "Failed to send email using MAPI.";
	text_t share_small_help =
		"Windows lists the apps that can receive the files. Originals are unchanged.";
	text_t share_zip = "Send the files as one zip file";
	text_t button_share = "&Share";
	text_t share_failed = "The files could not be shared.";
	text_t keyboard_or = "OR";
	text_t keyboard_alt = "alt";
	text_t keyboard_control = "ctrl";
	text_t keyboard_shift = "shift";
	text_t keyboard_backspace = "backspace";
	text_t open_with_fmt = "Open with {}";
	text_t open_with_title = "Open";
	text_t open_with_failed = "Failed to open selected items.";
	text_t tag_selected = "Tag selected items";
	text_t group_sort_tooltip = "Group and sort items";
	text_t repeat_help = "When the last item has played, start again from the first";
	text_t repeat_one_help = "Repeat play single media item";
	text_t repeat_off_help = "Stop after the last item";
	text_t update_available = "Update available";
	text_t update_avail_version_fmt = "Version {} of Diffractor is available.";
	text_t update_current_version_fmt = "You are currently using version {}.";
	text_t keyboard_accelerator_press = "press";
	text_t invalid = "invalid";
	text_t dates_title = "Dates";
	text_t dates_metadata_created = "Metadata created:";
	text_t dates_file_created = "File created:";
	text_t dates_file_modified = "File modified:";
	text_t stream_name_fmt = "Stream {}";
	text_t copy_to_join = "to";
	text_t save_new_photo = "Save new photo";
	text_t open_title = "Open";
	// Names the Add location command (`command_locate`) literally rather than by substitution.
	// Renaming that command means editing this sentence too - see locations.md 5.6.
	text_t map_instructions =
		"The crosshair at the centre of the map marks the location that will be written. Drag the map, click a photo bubble to move it under the crosshair, or search for a place by name. Use the mouse wheel to zoom. Nothing is written until you press Add location.";
	text_t command_filter_items = "Filter items";
	text_t command_filter_photos = "Photo filter";
	text_t command_filter_videos = "Video filter";
	text_t command_filter_audio = "Audio filter";
	text_t collection_contains = "The collection contains {} {} ({}).";
	text_t collection_contains2 = "The collection contains:";
	text_t items_created_fmt = "Items created {} {}";
	text_t items_modified_fmt = "Items modified {} {}";
	text_t open_created_modified = "Opens created items.\nCtrl+click opens modified items.";
	text_t history_navigator_action = "Shows the eight years ending here.";
	text_t click_items_from_fmt = "{} items close to {} in the collection.";
	text_t map_items_close_to_fmt = "{} items close to {}";
	text_t filter = "filter";

	text_t genre_a_capella = "A capella";
	text_t genre_abstract = "Abstract";
	text_t genre_acidjazz = "Acid Jazz";
	text_t genre_acidpunk = "Acid Punk";
	text_t genre_acid = "Acid";
	text_t genre_acoustic = "Acoustic";
	text_t genre_action_adventure = "Action & Adventure";
	text_t genre_action = "Action";
	text_t genre_aerial = "Aerial";
	text_t genre_alternative = "Alternative";
	text_t genre_alternrock = "AlternRock";
	text_t genre_ambient = "Ambient";
	text_t genre_analog = "Analog";
	text_t genre_animation = "Animation";
	text_t genre_anime = "Anime";
	text_t genre_architectural = "Architectural";
	text_t genre_avantgarde = "Avantgarde";
	text_t genre_aviation = "Aviation";
	text_t genre_ballad = "Ballad";
	text_t genre_bass = "Bass";
	text_t genre_bebob = "Bebob";
	text_t genre_bigband = "Big Band";
	text_t genre_bluegrass = "Bluegrass";
	text_t genre_blues = "Blues";
	text_t genre_booty_bass = "Booty Bass";
	text_t genre_brazilian = "Brazilian";
	text_t genre_cabaret = "Cabaret";
	text_t genre_candid = "Candid";
	text_t genre_celtic = "Celtic";
	text_t genre_chambermusic = "Chamber Music";
	text_t genre_chanson = "Chanson";
	text_t genre_childrens = "Children's";
	text_t genre_chorus = "Chorus";
	text_t genre_christian_gospel = "Christian & Gospel";
	text_t genre_christian_rap = "Christian Rap";
	text_t genre_classic_rock = "Classic Rock";
	text_t genre_classic = "Classic";
	text_t genre_classical = "Classical";
	text_t genre_close_up = "Close-up";
	text_t genre_cloudscape = "Cloudscape";
	text_t genre_club = "Club";
	text_t genre_comedy = "Comedy";
	text_t genre_conceptual = "Conceptual";
	text_t genre_concert_films = "Concert Films";
	text_t genre_concert = "Concert";
	text_t genre_conservation = "Conservation";
	text_t genre_country = "Country//genre";
	text_t genre_cult = "Cult";
	text_t genre_dance_hall = "Dance Hall";
	text_t genre_dance = "Dance";
	text_t genre_darkwave = "Darkwave";
	text_t genre_death_metal = "Death Metal";
	text_t genre_disco = "Disco";
	text_t genre_documentary = "Documentary";
	text_t genre_drama = "Drama";
	text_t genre_dream = "Dream";
	text_t genre_drum_solo = "Drum Solo";
	text_t genre_duet = "Duet";
	text_t genre_easylistening = "Easy Listening";
	text_t genre_electronic = "Electronic";
	text_t genre_ethnic = "Ethnic";
	text_t genre_euro_dance = "Eurodance";
	text_t genre_euro_house = "Euro-House";
	text_t genre_euro_techno = "Euro-Techno";
	text_t genre_family = "Family";
	text_t genre_fashion = "Fashion";
	text_t genre_fast_fusion = "Fast Fusion";
	text_t genre_film_still = "Film still";
	text_t genre_fine_art = "Fine-art";
	text_t genre_fire = "Fire";
	text_t genre_fireworks = "Fireworks";
	text_t genre_fitness_workout = "Fitness & Workout";
	text_t genre_folk = "Folk";
	text_t genre_folklore = "Folklore";
	text_t genre_folk_rock = "Folk-Rock";
	text_t genre_food = "Food";
	text_t genre_foreign = "Foreign";
	text_t genre_forensic = "Forensic";
	text_t genre_freestyle = "Freestyle";
	text_t genre_funk = "Funk";
	text_t genre_fusion = "Fusion";
	text_t genre_game = "Game//genre";
	text_t genre_gangsta = "Gangsta";
	text_t genre_geophotography = "Geo-photography";
	text_t genre_glamour = "Glamour";
	text_t genre_gospel = "Gospel";
	text_t genre_gothic_rock = "Gothic Rock";
	text_t genre_gothic = "Gothic";
	text_t genre_grunge = "Grunge";
	text_t genre_hardrock = "Hard Rock";
	text_t genre_high_key = "High key";
	text_t genre_high_speed = "High-speed";
	text_t genre_hip_hop = "Hip-Hop";
	text_t genre_hip_hop_rap = "Hip-Hop/Rap";
	text_t genre_holiday = "Holiday";
	text_t genre_horror = "Horror";
	text_t genre_house = "House";
	text_t genre_humour = "Humour";
	text_t genre_independent = "Independent";
	text_t genre_industrial = "Industrial";
	text_t genre_instrumental_pop = "Instrumental Pop";
	text_t genre_instrumental_rock = "Instrumental Rock";
	text_t genre_instrumental = "Instrumental";
	text_t genre_jazz = "Jazz";
	text_t genre_jazzfun = "Jazz + Funk";
	text_t genre_jungle = "Jungle";
	text_t genre_kids_family = "Kids & Family";
	text_t genre_kids = "Kids";
	text_t genre_kirlian = "Kirlian";
	text_t genre_landscape = "Landscape";
	text_t genre_latin = "Latin";
	text_t genre_lifestyle = "Lifestyle";
	text_t genre_lo_fi = "Lo-fi";
	text_t genre_lomography = "Lomography";
	text_t genre_long_exposure = "Long-exposure";
	text_t genre_low_key = "Low key";
	text_t genre_macro = "Macro";
	text_t genre_medical = "Medical";
	text_t genre_meditative = "Meditative";
	text_t genre_metal = "Metal";
	text_t genre_monochrome = "Monochrome";
	text_t genre_music_documentaries = "Music Documentaries";
	text_t genre_music_feature_films = "Music Feature Films";
	text_t genre_musical = "Musical";
	text_t genre_musicals = "Musicals";
	text_t genre_narrative = "Narrative";
	text_t genre_national_folk = "National Folk";
	text_t genre_native_american = "Native American";
	text_t genre_new_age = "New Age";
	text_t genre_new_wave = "New Wave";
	text_t genre_night = "Night";
	text_t genre_noise = "Noise";
	text_t genre_nonfiction = "Nonfiction";
	text_t genre_oldies = "Oldies";
	text_t genre_opera = "Opera";
	text_t genre_other = "Other";
	text_t genre_panorama = "Panorama";
	text_t genre_panoramic = "Panoramic";
	text_t genre_film_noir = "Film Noir";
	text_t genre_photo_op = "Photo op";
	text_t genre_photobiography = "Photobiography";
	text_t genre_photojournalism = "Photojournalism";
	text_t genre_photowalking = "Photowalking";
	text_t genre_podcast = "Podcast";
	text_t genre_polaroid = "Polaroid";
	text_t genre_polka = "Polka";
	text_t genre_pop = "Pop";
	text_t genre_pop_funk = "Pop/Funk";
	text_t genre_pop_folk = "Pop-Folk";
	text_t genre_porn_groove = "Porn Groove";
	text_t genre_portrait = "Portrait";
	text_t genre_power_ballad = "Power Ballad";
	text_t genre_pranks = "Pranks";
	text_t genre_primus = "Primus";
	text_t genre_progressive_rock = "Progressive Rock";
	text_t genre_psychadelic = "Psychadelic";
	text_t genre_psychedelic_rock = "Psychedelic Rock";
	text_t genre_punk_rock = "Punk Rock";
	text_t genre_punk = "Punk";
	text_t genre_randb = "R&B";
	text_t genre_rap = "Rap";
	text_t genre_rave = "Rave";
	text_t genre_reality_tv = "Reality TV";
	text_t genre_reggae = "Reggae";
	text_t genre_retro = "Retro";
	text_t genre_revival = "Revival";
	text_t genre_rhythmic_soul = "Rhythmic Soul";
	text_t genre_rock_and_roll = "Rock & Roll";
	text_t genre_rock = "Rock";
	text_t genre_romance = "Romance";
	text_t genre_samba = "Samba";
	text_t genre_satellite = "Satellite";
	text_t genre_satire = "Satire";
	text_t genre_scifi_and_fantasy = "Sci-Fi & Fantasy";
	text_t genre_short_films = "Short Films";
	text_t genre_show_tunes = "Show tunes";
	text_t genre_singer_songwriter = "Singer/Songwriter";
	text_t genre_ska = "Ska";
	text_t genre_slow_jam = "Slow Jam";
	text_t genre_slow_rock = "Slow Rock";
	text_t genre_social = "Social";
	text_t genre_soft_focus = "Soft focus";
	text_t genre_sonata = "Sonata";
	text_t genre_soul = "Soul";
	text_t genre_sound_clip = "Sound Clip";
	text_t genre_soundtrack = "Soundtrack";
	text_t genre_southern_rock = "Southern Rock";
	text_t genre_space = "Space";
	text_t genre_special_interest = "Special Interest";
	text_t genre_speech = "Speech";
	text_t genre_sports = "Sports";
	text_t genre_star_trail = "Star trail";
	text_t genre_still_life = "Still life";
	text_t genre_stock = "Stock";
	text_t genre_street = "Street";
	text_t genre_subminiature = "Subminiature";
	text_t genre_swing = "Swing";
	text_t genre_symphonic_rock = "Symphonic Rock";
	text_t genre_symphony = "Symphony";
	text_t genre_tango = "Tango";
	text_t genre_techno = "Techno";
	text_t genre_techno_industrial = "Techno-Industrial";
	text_t genre_teens = "Teens";
	text_t genre_thriller = "Thriller";
	text_t genre_time_lapse = "Time-lapse";
	text_t genre_top40 = "Top 40";
	text_t genre_trailer = "Trailer";
	text_t genre_trance = "Trance";
	text_t genre_travel = "Travel";
	text_t genre_tribal = "Tribal";
	text_t genre_trip_hop = "Trip-Hop";
	text_t genre_ultraviolet = "Ultraviolet";
	text_t genre_underwater = "Underwater";
	text_t genre_unknown = "Unknown";
	text_t genre_urban = "Urban";
	text_t genre_vernacular = "Vernacular";
	text_t genre_vintage = "Vintage";
	text_t genre_vocal = "Vocal";
	text_t genre_war = "War";
	text_t genre_western = "Western";
	text_t genre_world = "World";
	text_t search_last_7_days = "Last 7 days";
	text_t search_christmas = "Christmas";
	text_t total_title = "{} ({}) total items in {} folders.";
	text_t index_size_fmt = "Currently the index database is {} in size and contains metadata from {} items.";
	text_t count = "Count";
	text_t folder = "folder";
	text_t folders = "folders";
	text_t photo = "photo";
	text_t photos = "photos";
	text_t video = "video";
	text_t data = "data";
	text_t subtitle = "subtitle";
	text_t videos = "videos";
	text_t audio = "audio";
	text_t audio_title = "Audio";
	text_t archive = "archive";
	text_t archives = "archives";
	text_t retro = "retro";
	text_t retro_title = "Retro";
	text_t other = "other";
	text_t others = "others";
	text_t truncated_at_one_mb = "Truncated at 1MB";
	text_t help_more_info = "Help or more information";
	text_t help_send_info = "Send anonymous log information to Diffractor HQ.";
	text_t diagnostics_sent = "Diagnostics information was sent to Diffractor HQ.";
	text_t diagnostics_send_failed =
		"Diagnostics information could not be sent. Check your internet connection and try again.";
	text_t collection_title = "Your collection";
	text_t location_title = "Location";
	text_t size_title = "Size";
	text_t presence_tile = "Presence";
	text_t presence_loading = "Checking presence...";
	text_t presence_not_in = "Outside collection; no possible copy identified";
	text_t presence_this_in = "In collection";
	text_t presence_similar_in = "Possible copy in collection";
	text_t presence_newer_in = "Possible newer copy in collection";
	text_t presence_older_in = "Possible older copy in collection";
	text_t presence_not_in_long =
		"This item is outside the collection. No possible copy was identified in the collection.";
	text_t presence_this_in_long = "This item is in the collection.";
	text_t presence_similar_in_long =
		"This item is outside the collection. One or more possible copies exist in the collection.";
	text_t presence_newer_in_long =
		"This item is outside the collection. One or more possible newer copies exist in the collection.";
	text_t presence_older_in_long =
		"This item is outside the collection. One or more possible older copies exist in the collection.";
	text_t unselect_fmt = "Unselect {}";
	text_t delete_fmt = "Delete {}";
	text_t folder_title = "Folder";
	text_t command_save_and_back_tooltip = "Save the current item and open the previous item.";
	text_t command_save_and_next_tooltip = "Save the current item and open the next item.";
	text_t editing_title = "Editing";
	text_t edit_info =
		"Adjust the crop, rotation, perspective, and color of a photo. Nothing is written to disk until you save.";
	text_t movie_title = "Movie mixer";
	text_t movie_info =
		"Assemble the selected photos and videos into one video. Add, remove and reorder clips on the strip below, and trim the selected one. The preview is what will be rendered; nothing is written until you render or save.";
	text_t movie_settings_title = "Movie";
	text_t movie_clip_title = "Selected clip";
	text_t movie_crossfade = "Crossfade between clips";
	text_t movie_cut = "Cut between clips";
	text_t movie_transition_length = "Transition (tenths of a second)";
	// The same number sets the crossfade and the fades at the movie's ends, so it is labelled for
	// whichever of the two the current settings actually use.
	text_t movie_fade_length = "Fade (tenths of a second)";
	text_t movie_fade_in = "Fade in from black";
	text_t movie_fade_out = "Fade out to black";
	text_t movie_photo_duration = "Photo duration (tenths of a second)";
	text_t movie_clip_duration = "This photo (tenths of a second)";
	text_t movie_use_default = "Use the movie default";
	text_t movie_empty = "Add photos and videos to build a movie.";
	text_t movie_output_fmt = "Output {} x {} at {} fps, {} long.";
	text_t movie_clip_fmt = "{} - clip {} of {}, starting at {}.";
	text_t movie_project_files = "Movie project";
	text_t movie_maker_files = "Windows Movie Maker project";
	text_t movie_media_files = "Photos and videos";
	text_t movie_open_failed_fmt = "Could not read the project {}.";
	text_t movie_save_failed_fmt = "Could not save the project {}.";
	text_t movie_video_files = "Video";
	text_t movie_rendering = "Rendering the movie";
	text_t movie_render_failed_fmt = "The movie could not be rendered. {}";
	text_t movie_error_no_length = "The movie has no length.";
	text_t movie_error_no_encoder = "The encoder would not accept this movie.";
	text_t movie_error_no_memory = "There was not enough memory for a movie frame.";
	text_t movie_error_undecodable = "A clip could not be decoded.";
	text_t movie_error_encoder_failed = "The encoder stopped before the movie was finished.";
	text_t movie_error_output_is_source = "That name belongs to a clip in this movie. Choose another name.";
	text_t movie_rendered_fmt = "Rendered {}.";
	text_t movie_render_cancelled = "Rendering was canceled. Nothing was written.";
	text_t movie_relink_none = "No missing clip was found in that folder.";
	text_t movie_unsaved_prompt =
		"This movie has changes that are not in a project file. It is kept while Diffractor is running, and closing Diffractor discards it.";
	text_t movie_discard = "Close without saving";
	text_t command_movie = "Movie mixer";
	text_t tooltip_movie = "Assemble the selected photos and videos into one video.";
	text_t command_movie_render = "Render";
	text_t tooltip_movie_render = "Write the movie to a video file.";
	text_t command_movie_relink = "Relink missing files";
	text_t tooltip_movie_relink = "Point clips whose source file has moved at a folder you choose.";
	text_t command_movie_add = "Add files";
	text_t command_movie_remove = "Remove from movie";
	text_t command_movie_send_to_end = "Send to the end";
	text_t command_movie_open = "Open project";
	text_t command_movie_save = "Save project";
	text_t command_movie_import = "Import Movie Maker project";
	text_t convert_info =
		"Convert the selected photos to another format and optionally limit their size. The originals are kept and new files are written to the destination folder.";
	text_t edit_metadata_info =
		"Write the same metadata values to every selected item. Only the fields you check are changed; image and video data is untouched.";
	text_t adjust_date_info =
		"Shift the dates of the selected items so the earliest item starts at a new date and time. Only metadata is changed; image and video data is untouched.";
	text_t tag_info =
		"Add or remove tags across the selected items. Tags you do not mention are left in place.";
	text_t add_folder = "Add folder";
	text_t items_identical = "Files are identical.";
	text_t items_not_identical = "Files are different.";
	text_t pixels_identical_files_not_identical = "Pixels are identical but files are different.";
	text_t copy_to_clipboard = "Copy to clipboard";
	text_t open_link_fmt = "Open link {}";
	text_t open_link_choose = "Choose a link to open";
	text_t duplicate_text = "Duplicate";
	text_t xmp_metadata_title = "XMP";
	text_t icc_metadata_title = "ICC";
	text_t metadata_title = "Metadata";
	text_t media_metadata_title = "Media";
	text_t raw_metadata_title = "Raw";
	text_t exif_metadata_title = "EXIF";
	text_t iptc_metadata_title = "IPTC";
	text_t none = "None";
	text_t collection_options_info = "Choose the folders that make up your collection. Your files stay where they are.";
	text_t more_collection_options_information = "More information about collection management.";
	text_t more_template_information = "More information about metadata templates.";
	text_t show_raw_now = "Show this RAW photo";
	text_t show_raw = "Always show RAW photos (slower)";
	text_t preview_show_preview = "Show RAW preview if available (faster)";
	text_t preview_showing = "Showing a RAW photo preview.";
	text_t preview_rendering = "Rendering RAW photo...";
	text_t preview_rendered = "Showing fully rendered RAW photo.";

	text_t command_favorite = "Favorite";
	text_t command_advanced_search = "Advanced Search";
	text_t favorite_add_fmt = "Add '{}' to favorites.";
	text_t favorite_remove_fmt = "Remove '{}' from favorite folders or searches.";
	text_t collection_in = "These items are in your collection.";
	text_t collection_not_in =
		"These items are outside your collection, so search and duplicate detection do not cover them. Add this folder to your collection to include them.";
	text_t collection_info =
		"Your collection is the folders you choose, including their subfolders. Diffractor indexes them so search, duplicate detection and totals cover everything at once. Adding or removing a folder changes only what Diffractor knows. No files are moved or copied.";
	text_t favorite_title = "Favorite title";
	text_t favorite_info = "Favorites are listed in the sidebar for quick access.";
	text_t favorite_failed_to_add = "Failed to add. Maximum number of favorites reached.";

	text_t search_collection = "Search in the collection";
	text_t search_folder = "Search in a folder";
	text_t search_sub_folders = "Search in subfolders";
	text_t search_all_terms = "All these terms";
	text_t search_none_terms = "None of these terms";
	text_t search_photos = "Photos";
	text_t search_videos = "Videos";
	text_t search_audio = "Audio";
	text_t search_located_within = "Located within";
	text_t search_select_term = "Select term";
	text_t search_no_criteria = "Select at least one search criterion.";
	text_t search_date_from = "From";
	text_t search_date_until = "Until";
	text_t km_from = "km from";

	text_t command_sync = "Synchronize";
	text_t sync_collection = "Synchronize whole collection";
	text_t sync_other_folder = "Synchronize only one folder";
	text_t sync_local_remote = "Copy newer local files to remote folders";
	text_t sync_remote_local = "Copy newer remote files to local folders";
	text_t sync_delete_local = "Delete local files that do not exist remotely";
	text_t sync_delete_remote = "Delete remote files that do not exist locally";
	text_t sync_local = "Local folders";
	text_t sync_info_1 =
		"Synchronize local folders with a remote or backup folder. Analyze first to review every copy and delete before you run.";
	text_t sync_info_2 = "c:\\photos with \\\\nas\\\\backup";
	text_t sync_remote = "Remote folder";
	text_t sync_copy_remote_action = "copy remote";
	text_t sync_copy_local_action = "copy local";
	text_t sync_delete_remote_action = "delete remote";
	text_t sync_delete_local_action = "delete local";
	text_t sync_preparing = "Preparing to synchronize files...";
	text_t sync_analysis_changed = "Files changed after analysis. Analyze again to review the updated actions.";
	text_t sync_delete_warning =
		"Synchronization will permanently delete files. These files cannot be recovered from the Recycle Bin.";
	text_t sync_delete_count_fmt = "Local files to delete: {}. Remote files to delete: {}.";
	text_t button_sync = "&Synchronize";

	text_t import_from = "Source of import:";
	text_t import_other_folder = "Other folder";
	text_t button_analyze = "&Analyze";
	text_t analyze = "Analyze";
	text_t select_location = "Select a location";
	text_t selected_items_fmt = "Selected items [{}]";
	text_t analyzing = "Analyzing...";
	text_t processing = "Processing...";
	text_t processing_files = "Processing files";
	text_t scope = "Scope";
	text_t value = "Value";
	text_t some_items_filtered_fmt = "Show {} filtered out items";
	text_t items_created_on_fmt = "The collection contains {} items created on {}.";

	text_t command_all_tags = "Show all tags";
	text_t command_favorite_tags = "Show only favorite tags";
	text_t option_favorite_tags = "Show only favorite tags in the sidebar";

	text_t file = "File";
	text_t source = "Source";
	text_t destination = "Destination";
	text_t action = "Action";
	text_t old_name = "Old name";
	text_t new_name = "New name";
	text_t local = "Local";
	text_t remote = "Remote";
	text_t status = "Status";
	text_t message = "Message";
	text_t import = "import";
	text_t exists = "exists";
	text_t previously_imported = "previously imported";
	text_t ignore = "ignore";
	text_t view_empty_message = "Click analyze to show items";
	text_t no_items_selected_message = "Select items before using this tool";
	text_t tags_view_empty_message = "Select items to tag";

	text_t command_cancel_operation = "Cancel";
	text_t cancel_operation_title = "Cancel operation";
	text_t cancel_operation_fmt = "{} is still running. Cancel it and close?";
	text_t button_cancel_operation = "Cancel operation";
	text_t button_keep_running = "Keep running";

	text_t scope_unavailable_title = "Location unavailable";
	text_t scope_unavailable_fmt =
		"{} could not be opened. It may be disconnected, renamed or removed.";
	text_t scope_unavailable_retry = "Try again";
	text_t scope_unavailable_parent = "Go to the parent folder";
	text_t scope_busy_fmt = "{} is still running. Close it before browsing somewhere else.";

	text_t safe_start_title = "Recovered from a startup problem";
	text_t safe_start_message =
		"Diffractor closed unexpectedly before it finished starting, so this run uses the default layout with hardware acceleration turned off. Your files, collection and recent items are unchanged. You can restore your settings from Options.";

	text_t collision_policy_label = "If the destination already exists:";
	text_t collision_replace = "Replace";
	text_t collision_skip = "Skip";
	text_t collision_rename = "Auto-rename";
	text_t collision_block = "Block run";
	// Row label stating the fact only. The policy control, not the row, asks what to do about it.
	text_t collision_exists = "File exists";
	text_t collision_blocked_fmt = "{} destinations already exist. Choose Replace, Skip or Auto-rename.";
	text_t collision_skipped_fmt = "{} items will be skipped because the destination exists.";
	text_t collision_replaced_fmt = "{} existing destinations will be replaced.";
	text_t collision_renamed_fmt = "{} items will be given a new name to avoid a collision.";

	// Row labels for a finished run. The plan words are future tense; these are what happened.
	text_t result_success = "Success";
	text_t result_failed = "Failed";
	text_t result_skipped = "Skipped";
	text_t result_not_run = "Not run";
	text_t run_needs_refresh = "These are the results of a finished run. Refresh to plan again with the current settings.";

	plural_text rotate_info_fmt = {
		"Rotate {first-name}.", "Rotate {count} selected items. Originals will be overwritten."
	};
	plural_text title_folder_count_fmt = {"1 folder", "{count} folders"};
	plural_text title_item_count_fmt = {"{count} item", "{count} items"};
	plural_text map_items_here_fmt = {"1 item here", "{count} items here"};
	plural_text rating_set_fmt = {"Set rating to 1 star", "Set rating to {count} stars"};
	plural_text cannot_process_fmt = {
		"Cannot process {first-name}.", "Cannot process {first-name} and {other} other items."
	};
	plural_text rename_fmt = {
		"{first-name} will be renamed.", "{count} items will be renamed."
	};
	plural_text dup_count_fmt = {"1 duplicate item", "{count} duplicate items"};
	plural_text sidecar_count_fmt = {"1 sidecar item", "{count} sidecar items"};
	plural_text processed_x_of_x_fmt = {
		"{count} of 1 item was processed.", "{count} of {total} items were processed."
	};
	plural_text processed_fmt = {"1 item was processed.", "{count} items were processed."};
	plural_text failed_items_fmt = {"{first-name} failed.", "{count} items failed."};
	plural_text ignored_fmt = {"{first-name} was ignored.", "{count} items were ignored."};
	plural_text canceled_items_fmt = {"{first-name} was canceled.", "{count} items were canceled."};
	plural_text ignored_exist_already_fmt = {
		"{first-name} was ignored because it exists already in the destination location.",
		"{count} items were ignored because they exist already in the destination location."
	};
	plural_text ignored_previous_fmt = {
		"{first-name} was ignored because it was previously imported.",
		"{count} items were ignored because they were previously imported."
	};
	plural_text import_date_unset_fmt = {
		"1 item was imported but its created date could not be set.",
		"{count} items were imported but their created date could not be set."
	};
	plural_text delete_info_fmt = {
		"{first-name} will be moved to the recycle bin when Windows can recycle it.",
		"{count} items ({size}) will be moved to the recycle bin when Windows can recycle them."
	};
	plural_text delete_info_permanent_fmt = {
		"{first-name} will be permanently deleted.",
		"{count} items ({size}) will be permanently deleted."
	};
	plural_text delete_many_warning_fmt = {
		"You are about to delete {count} item.",
		"You are about to delete {count} items."
	};
	text_t delete_no_recycle_warning =
		"These items are on a network path. They will be permanently deleted and cannot be recovered from the recycle bin.";
	plural_text copy_fmt = {"{first-name} will be copied.", "{count} items ({size}) will be copied."};
	plural_text move_fmt = {"{first-name} will be moved.", "{count} items ({size}) will be moved."};
	plural_text be_updated_fmt = {"{first-name} will be updated.", "{count} items will be updated."};
	plural_text edit_metadata_fmt = {
		"Add or overwrite specific metadata in {first-name}.",
		"Add or overwrite specific metadata in {count} items."
	};
	plural_text burn_info_fmt = {
		"{first-name} will be staged for disc burning.",
		"{count} selected items ({size}) will be staged for disc burning."
	};
	plural_text convert_info_fmt = {
		"{first-name} will be converted and copied to a folder.",
		"{count} items will be converted and copied to a folder."
	};
	plural_text tag_info_fmt = {
		"{first-name} will have tags added or removed.", "{count} items will have tags added or removed."
	};
	plural_text adjust_date_info_fmt = {
		"{first-name} will have the date adjusted.", "{count} items will have the date adjusted."
	};
	plural_text email_info_fmt = {
		"A desktop email draft will be opened with {first-name} attached.",
		"A desktop email draft will be opened with {count} selected items attached."
	};
	plural_text share_info_fmt = {
		"{first-name} will be offered to the app you choose.",
		"{count} selected items will be offered to the app you choose."
	};
	plural_text would_overwrite_fmt = {
		"{first-name} already exists. Do you want to replace it?",
		"{first-name} and {other} other items already exist. Do you want to replace them?"
	};
	plural_text gps_overwrite_count_fmt = {
		"{first-name} will have existing GPS position metadata overwritten.",
		"{first-name} and {other} other items will have existing GPS position metadata overwritten."
	};
	plural_text movie_ignored_fmt = {
		"1 element of the imported project was left out because Movie cannot represent it.",
		"{count} elements of the imported project were left out because Movie cannot represent them."
	};
	plural_text movie_missing_fmt = {
		"1 clip cannot find its source file.", "{count} clips cannot find their source files."
	};
	plural_text movie_relink_fmt = {"1 clip was relinked.", "{count} clips were relinked."};
};

extern app_text_t tt;
