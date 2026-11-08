// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The Options view: every setting the user can change, on one page that fills the window, in the
// sections a user looks for - the collection first, then viewing and playback, the sidebar, editing and
// saving, and general settings - each under its own shaded header, with its cards in as many columns as fit.
// It edits a copy of the settings; Save options and close commits only what the page edits, and asks for the
// work a change needs - a re-index, a language, an audio device - while Cancel discards the copy, and Close,
// Escape and closing Diffractor ask before they discard a change.

#include "pch.h"
#include "view_options.h"
#include "app_commands.h"
#include "app_dialogs.h"
#include "app_text.h"

namespace
{
	constexpr auto options_docs_url = "https://www.diffractor.com/docs";

	// Each set of radio buttons on the page needs its own group; see ui::radio_group_default.
	constexpr int radio_group_subtitles = 20;
	constexpr int radio_group_repeat = 21;
	constexpr int radio_group_navigator = 22;

	constexpr prop::library_kind all_library_kinds[] = {
		prop::library_kind::automatic, prop::library_kind::personal, prop::library_kind::films_tv,
		prop::library_kind::music
	};

	// Offered for subtitles beyond the app's own languages: the languages films are most often subtitled in.
	constexpr std::string_view subtitle_languages[] = {
		"en", "ar", "cs", "da", "de", "el", "es", "fi", "fr", "he", "hi", "hu", "it", "ja", "ko", "nl", "no", "pl",
		"pt", "ro", "ru", "sv", "tr", "uk", "zh"
	};

	std::string_view library_kind_name(const prop::library_kind kind)
	{
		switch (kind)
		{
		case prop::library_kind::personal: return tt.library_kind_personal;
		case prop::library_kind::films_tv: return tt.library_kind_films_tv;
		case prop::library_kind::music: return tt.library_kind_music;
		case prop::library_kind::automatic: break;
		}

		return tt.library_kind_automatic;
	}

	std::string subtitle_language_name(const std::string_view code)
	{
		return code.empty() ? std::string(tt.subtitle_language_app.sv()) : language_name(code);
	}

	std::string sound_device_name(const std::string_view id, const std::vector<platform::sound_device>& devices)
	{
		const auto found = std::ranges::find(devices, id, &platform::sound_device::id);
		return found != devices.end() ? found->name : std::string(tt.audio_device_default.sv());
	}

	// The app's languages: English, and each translation installed beside it.
	std::vector<std::string> installed_languages()
	{
		std::vector<std::string> result = {"en"};
		const auto folder = platform::known_path(platform::known_folder::running_app_folder).combine("languages");

		for (const auto& f : platform::iterate_file_items(folder, false).files)
		{
			const auto path = folder.combine_file(f.name);
			if (str::icmp(path.extension(), ".po") == 0) result.emplace_back(path.file_name_without_extension());
		}

		return result;
	}

	// Sections in as many columns as the width allows, each placed in the column that is shortest so far, so
	// sections of very different heights pack without leaving gaps.
	class options_columns final : public view_elements
	{
	public:
		sizei measure(ui::measure_context& mc, const int width_limit) const override
		{
			return {width_limit, arrange(mc, recti(0, 0, width_limit, 0), false)};
		}

		void layout(ui::measure_context& mc, const recti bounds_in) override
		{
			update_style_from_children();
			bounds = bounds_in;
			arrange(mc, bounds_in, true);
		}

	private:
		int arrange(ui::measure_context& mc, const recti area, const bool place) const
		{
			const auto gap = mc.padding2 * 4;
			const auto min_width = df::round(340 * mc.scale_factor);
			const auto columns = std::clamp((area.width() + gap) / (min_width + gap), 1, 4);
			const auto width = (area.width() - gap * (columns - 1)) / columns;
			std::vector<int> heights(columns, 0);

			for (const auto& c : _children)
			{
				if (!c->is_visible()) continue;

				const auto column = static_cast<int>(std::ranges::min_element(heights) - heights.begin());
				const auto margin = c->margin * mc.scale_factor;
				const auto extent = c->measure(mc, width - margin.cx * 2);
				const auto left = area.left + column * (width + gap);
				const auto top = area.top + heights[column];

				if (place)
				{
					c->layout(mc, recti(left + margin.cx, top + margin.cy, left + width - margin.cx,
					                    top + margin.cy + extent.cy));
				}

				heights[column] += extent.cy + margin.cy * 2 + gap;
			}

			const auto tallest = std::ranges::max(heights);
			return tallest > 0 ? tallest - gap : 0;
		}
	};

	// A section's header: its icon, its name and what it covers, on a shaded band like the explainer that opens
	// a task view's panel, so each section stands apart from the cards of the one before without a rule between
	// them. The band is the element's bounds, so a section scrolled to the top shows all of it.
	class options_section_header final : public view_element
	{
		icon_index _icon = icon_index::none;
		std::string _title;
		std::string _info;
		mutable sizei _title_extent;

		static int inset(const ui::measure_context& mc)
		{
			return df::round(12 * mc.scale_factor);
		}

	public:
		options_section_header(const icon_index icon, const std::string_view title, const std::string_view info) :
			view_element(view_element_style::shaded_background), _icon(icon), _title(title), _info(info)
		{
			padding = {0, 0};
			margin = {8, 8};
			update_background_color();
		}

		sizei measure(ui::measure_context& mc, const int width_limit) const override
		{
			const auto text_width = std::max(0, width_limit - inset(mc) * 2 - mc.icon_cxy * 2);
			_title_extent = mc.measure_text(_title, ui::style::font_face::title, ui::style::text_style::multiline,
			                                text_width);
			const auto info_extent = mc.measure_text(_info, ui::style::font_face::dialog,
			                                         ui::style::text_style::multiline, text_width);
			return {width_limit, _title_extent.cy + info_extent.cy + inset(mc) * 2};
		}

		void layout(ui::measure_context& mc, const recti bounds_in) override
		{
			bounds = bounds_in;
			measure(mc, bounds_in.width());
		}

		void render(ui::draw_context& dc, const pointi element_offset) const override
		{
			render_background(dc, element_offset);

			const auto content = bounds.offset(element_offset).inflate(-inset(dc));
			const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);
			const auto text_left = content.left + dc.icon_cxy * 2;
			const auto title_bottom = content.top + _title_extent.cy;

			xdraw_icon(dc, _icon, recti(content.left, content.top, text_left, title_bottom), clr, {});
			dc.draw_text(_title, recti(text_left, content.top, content.right, title_bottom),
			             ui::style::font_face::title, ui::style::text_style::multiline, clr, {});
			dc.draw_text(_info, recti(text_left, title_bottom, content.right, content.bottom),
			             ui::style::font_face::dialog, ui::style::text_style::multiline, clr, {});
		}
	};

	std::shared_ptr<ui::group_control> make_section(const std::string_view title)
	{
		auto result = std::make_shared<ui::group_control>();
		result->add(std::make_shared<ui::title_control>(title));
		return result;
	}

	// A choice of one value from a menu, with what is chosen beside the button that opens it.
	std::shared_ptr<ui::select_control> make_chooser(const ui::frame_ptr& frame, std::string& text,
	                                                 std::function<std::vector<ui::command_ptr>(
		                                                 const std::shared_ptr<ui::select_control>&)> commands)
	{
		return std::make_shared<ui::select_control>(frame, std::string_view{}, text, std::move(commands),
		                                            icon_index::down);
	}
}

std::vector<df::folder_path> library_type_folders(const settings_t::index_t& collection,
                                                  const local_folders_result& local)
{
	std::vector<df::folder_path> result;

	const auto add = [&result](const df::folder_path folder)
	{
		if (folder.is_empty()) return;
		if (std::ranges::any_of(result, [folder](const df::folder_path f) { return f == folder; })) return;
		result.emplace_back(folder);
	};

	if (collection.pictures) add(local.pictures);
	if (collection.video) add(local.video);
	if (collection.music) add(local.music);
	if (collection.drop_box) add(local.dropbox_photos);
	if (collection.onedrive_pictures) add(local.onedrive_pictures);
	if (collection.onedrive_video) add(local.onedrive_video);
	if (collection.onedrive_music) add(local.onedrive_music);

	for (const auto line : split_collection_folders(collection.more_folders))
	{
		const auto folder = str::trim(line);
		if (!folder.empty() && !str::is_exclude(folder) && df::is_path(folder)) add(df::folder_path(folder));
	}

	for (const auto& [folder, kind] : parse_library_types(collection.library_types)) add(folder);

	return result;
}

// Every setting the page edits, in pairs: a field of `from` and the same field of `to`. Saving copies each pair
// and asking whether anything changed compares each, so neither can miss a setting the other has.
template <typename To, typename Visit>
static void visit_edited_options(const settings_t& from, To& to, Visit&& visit)
{
	// Viewing
	visit(from.show_rotated, to.show_rotated);
	visit(from.scale_up, to.scale_up);
	visit(from.highlight_large_items, to.highlight_large_items);
	visit(from.sort_dates_descending, to.sort_dates_descending);
	visit(from.large_font, to.large_font);
	visit(from.show_shadow, to.show_shadow);
	visit(from.raw_preview, to.raw_preview);
	visit(from.verbose_metadata, to.verbose_metadata);
	visit(from.zoom_navigator, to.zoom_navigator);

	// Application
	visit(from.show_hidden, to.show_hidden);
	visit(from.confirm_deletions, to.confirm_deletions);
	visit(from.confirm_rotations, to.confirm_rotations);
	visit(from.show_help_tooltips, to.show_help_tooltips);
	visit(from.language, to.language);

	// Playback
	visit(from.auto_play, to.auto_play);
	visit(from.auto_advance, to.auto_advance);
	visit(from.last_played_pos, to.last_played_pos);
	visit(from.repeat, to.repeat);
	visit(from.sound_device, to.sound_device);
	visit(from.slideshow_delay, to.slideshow_delay);
	visit(from.subtitles, to.subtitles);
	visit(from.subtitle_language, to.subtitle_language);

	// Collection
	visit(from.collection, to.collection);
	visit(from.read_nfo, to.read_nfo);

	// Sidebar
	visit(from.show_sidebar, to.show_sidebar);
	visit(from.sidebar.show_total_items, to.sidebar.show_total_items);
	visit(from.sidebar.show_history, to.sidebar.show_history);
	visit(from.sidebar.show_world_map, to.sidebar.show_world_map);
	visit(from.sidebar.show_drives, to.sidebar.show_drives);
	visit(from.sidebar.show_favorite_searches, to.sidebar.show_favorite_searches);
	visit(from.sidebar.show_tags, to.sidebar.show_tags);
	visit(from.sidebar.show_ratings, to.sidebar.show_ratings);
	visit(from.sidebar.show_labels, to.sidebar.show_labels);
	visit(from.sidebar.show_favorite_tags_only, to.sidebar.show_favorite_tags_only);
	visit(from.sidebar.history_start_year, to.sidebar.history_start_year);
	visit(from.search, to.search);
	visit(from.favorite_tags, to.favorite_tags);

	// Saving
	visit(from.create_originals, to.create_originals);
	visit(from.jpeg_save_quality, to.jpeg_save_quality);
	visit(from.webp_quality, to.webp_quality);
	visit(from.webp_lossless, to.webp_lossless);
	visit(from.jxl_quality, to.jxl_quality);
	visit(from.jxl_lossless, to.jxl_lossless);

	// Updates and advanced
#ifndef WINSTORE
	visit(from.check_for_updates, to.check_for_updates);
	visit(from.send_crash_dumps, to.send_crash_dumps);
#endif
	visit(from.use_gpu, to.use_gpu);
	visit(from.use_d3d11va, to.use_d3d11va);
	visit(from.show_debug_info, to.show_debug_info);
}

void apply_edited_options(const settings_t& from, settings_t& to)
{
	visit_edited_options(from, to, [](const auto& field_from, auto& field_to) { field_to = field_from; });
	to.slideshow_delay = std::clamp(to.slideshow_delay, settings_t::min_slideshow_delay,
	                                settings_t::max_slideshow_delay);
}

bool edited_options_differ(const settings_t& a, const settings_t& b)
{
	auto differ = false;
	visit_edited_options(a, b, [&differ](const auto& field_a, const auto& field_b)
	{
		differ = differ || !(field_a == field_b);
	});
	return differ;
}

// The page's copy as Save stores it: its text forms put back into what is stored.
static settings_t stored_form(const options_view::edit_state& edit)
{
	auto result = edit.settings;
	result.collection.more_folders = str::combine(split_collection_folders(edit.more_folders), "\n", true);
	result.collection.library_types = format_library_types(edit.library_types);
	result.subtitles = edit.subtitles_off
		                   ? subtitle_mode::off
		                   : edit.subtitles_always
		                   ? subtitle_mode::always
		                   : subtitle_mode::foreign;
	result.repeat = edit.repeat_one
		                ? repeat_mode::repeat_one
		                : edit.repeat_all
		                ? repeat_mode::repeat_all
		                : repeat_mode::repeat_none;
	result.zoom_navigator = edit.navigator_off ? zoom_navigator_mode::off : zoom_navigator_mode::auto_hide;
	return result;
}

void options_view::open(const options_section section)
{
	// Its commands are disabled while the page shows, and a key pressed before that lands must not swap the copy
	// its controls edit for one that Save would commit unedited.
	if (_state.view_mode() == view_type::options) return;

	auto edit = std::make_shared<edit_state>();
	edit->settings = setting;

	const auto& s = edit->settings;
	edit->more_folders = str::combine(split_collection_folders(s.collection.more_folders), "\r\n", false);

	const auto kinds = parse_library_types(s.collection.library_types);

	for (const auto& folder : library_type_folders(s.collection, platform::local_folders()))
	{
		const auto found = std::ranges::find(kinds, folder, &std::pair<df::folder_path, prop::library_kind>::first);
		edit->library_types.emplace_back(folder, found != kinds.end() ? found->second : prop::library_kind::automatic);
	}

	// Sized once, before any control binds to an element.
	for (const auto& [folder, kind] : edit->library_types) edit->library_type_names.emplace_back(library_kind_name(kind));

	edit->language_name = language_name(s.language);
	edit->subtitle_language_name = subtitle_language_name(s.subtitle_language);
	edit->sound_device_name = sound_device_name(s.sound_device, platform::list_audio_playback_devices());
	edit->subtitles_off = s.subtitles == subtitle_mode::off;
	edit->subtitles_always = s.subtitles == subtitle_mode::always;
	edit->subtitles_foreign = s.subtitles == subtitle_mode::foreign;
	edit->repeat_none = s.repeat == repeat_mode::repeat_none;
	edit->repeat_one = s.repeat == repeat_mode::repeat_one;
	edit->repeat_all = s.repeat == repeat_mode::repeat_all;
	edit->navigator_auto_hide = s.zoom_navigator != zoom_navigator_mode::off;
	edit->navigator_off = s.zoom_navigator == zoom_navigator_mode::off;

	_opened = std::make_shared<const settings_t>(stored_form(*edit));
	_edit = std::move(edit);
	_section = section;
	if (_actions.pause_media) _media_pause = _actions.pause_media();
	_state.view_mode(view_type::options);
}

void options_view::close()
{
	_state.view_mode(view_type::items);
}

void options_view::cancel()
{
	close();
}

bool options_view::has_changes() const
{
	return _edit && _opened && edited_options_differ(stored_form(*_edit), *_opened);
}

bool options_view::leave()
{
	const auto choice = has_changes() && _actions.ask_to_save ? _actions.ask_to_save() : options_leave_choice::discard;

	if (choice == options_leave_choice::stay) return false;
	if (choice == options_leave_choice::save) save();
	else close();
	return true;
}

options_leave_choice ask_to_save_options(const ui::frame_ptr& parent)
{
	auto answer = std::make_shared<options_leave_choice>(options_leave_choice::stay);
	const auto dlg = make_dlg(parent);
	const auto frame = dlg->_frame;

	const std::vector<view_element_ptr> controls = {
		set_margin(std::make_shared<ui::title_control2>(frame, icon_index::settings, tt.options_title,
		                                               tt.options_unsaved_prompt)),
		std::make_shared<divider_element>(),
		std::make_shared<ui::button_control>(frame, icon_index::save, tt.command_options_save, std::string_view{},
		                                     [answer, frame]
		                                     {
			                                     *answer = options_leave_choice::save;
			                                     frame->close(false);
		                                     }),
		std::make_shared<ui::button_control>(frame, icon_index::del, tt.close_without_saving, std::string_view{},
		                                     [answer, frame]
		                                     {
			                                     *answer = options_leave_choice::discard;
			                                     frame->close(false);
		                                     }),
		std::make_shared<divider_element>(),
		std::make_shared<ui::close_control>(frame, true, tt.button_cancel),
	};

	if (dlg->show_modal(controls, {44}, {44}) != ui::close_result::ok) return options_leave_choice::stay;
	return *answer;
}

void options_view::help()
{
	platform::open(options_docs_url);
}

void options_view::save()
{
	if (!_edit)
	{
		cancel();
		return;
	}

	const auto edited = stored_form(*_edit);

	const auto collection_changed = edited.collection.pictures != setting.collection.pictures ||
		edited.collection.video != setting.collection.video ||
		edited.collection.music != setting.collection.music ||
		edited.collection.drop_box != setting.collection.drop_box ||
		edited.collection.onedrive_pictures != setting.collection.onedrive_pictures ||
		edited.collection.onedrive_video != setting.collection.onedrive_video ||
		edited.collection.onedrive_music != setting.collection.onedrive_music ||
		edited.collection.more_folders != setting.collection.more_folders ||
		edited.collection.library_types != setting.collection.library_types;
	const auto language_changed = edited.language != setting.language;
	const auto sound_device_changed = edited.sound_device != setting.sound_device;
	const auto read_nfo_changed = edited.read_nfo != setting.read_nfo;

	apply_edited_options(edited, setting);

	if (sound_device_changed) _state.change_audio_device(setting.sound_device);

	// Only a change re-walks the collection, as it always has. An .nfo is read as a video is scanned, so a
	// change to whether they are read has the collection read again.
	auto invalid = view_invalid::options | view_invalid::media_elements;
	if (collection_changed) invalid = invalid | view_invalid::index;
	if (read_nfo_changed) invalid = invalid | view_invalid::index_rebuild;
	_state.invalidate_view(invalid);

	close();

	if (language_changed && _actions.select_language) _actions.select_language(setting.language);
}

std::string_view options_view::title()
{
	_title = std::format("{}: {}", s_app_name, tt.options_title.sv());
	return _title;
}

view_bars options_view::bars() const
{
	return task_view_bars({commands::options_help, commands::options_cancel, commands::options_save});
}

view_controls_host_ptr options_view::controls(const ui::frame_ptr& owner)
{
	auto result = std::make_shared<view_controls_host>(_state);
	const auto& frame = owner;
	result->_frame = frame;

	if (!_edit) return result;

	const auto edit = _edit;
	auto& s = edit->settings;
	const auto local = platform::local_folders();

	// Viewing
	const auto viewing = make_section(tt.options_viewing_title);
	viewing->add(std::make_shared<ui::check_control>(frame, tt.options_show_rotated, s.show_rotated));
	viewing->add(std::make_shared<ui::check_control>(frame, tt.command_scale_up, s.scale_up));
	viewing->add(std::make_shared<ui::check_control>(frame, tt.command_highlight_large_items, s.highlight_large_items));
	viewing->add(std::make_shared<ui::check_control>(frame, tt.command_sort_dates_descending, s.sort_dates_descending));
	viewing->add(std::make_shared<ui::check_control>(frame, tt.command_view_large_font, s.large_font));
	viewing->add(std::make_shared<ui::check_control>(frame, tt.options_show_shadow, s.show_shadow));
	viewing->add(std::make_shared<ui::check_control>(frame, tt.preview_show_preview, s.raw_preview));
	viewing->add(std::make_shared<ui::check_control>(frame, tt.show_verbose_metadata, s.verbose_metadata));
	viewing->add(std::make_shared<text_element>(tt.command_zoom_navigator));
	viewing->add(std::make_shared<ui::check_control>(frame, tt.command_zoom_navigator_auto_hide,
	                                                 edit->navigator_auto_hide, true, false, nullptr,
	                                                 radio_group_navigator));
	viewing->add(std::make_shared<ui::check_control>(frame, tt.command_zoom_navigator_off, edit->navigator_off, true,
	                                                 false, nullptr, radio_group_navigator));

	// Application
	const auto application = make_section(tt.options_app_options);
	application->add(std::make_shared<text_element>(tt.command_language));
	application->add(make_chooser(frame, edit->language_name, [edit](const std::shared_ptr<ui::select_control>& sel)
	{
		std::vector<ui::command_ptr> result;

		for (const auto& code : installed_languages())
		{
			auto c = std::make_shared<ui::command>();
			c->text = language_name(code);
			c->checked = edit->settings.language == code;
			c->invoke = [edit, sel, code]
			{
				edit->settings.language = code;
				sel->update_text(language_name(code));
			};
			result.emplace_back(std::move(c));
		}

		return result;
	}));
	application->add(std::make_shared<ui::check_control>(frame, tt.options_show_hidden, s.show_hidden));
	application->add(std::make_shared<ui::check_control>(frame, tt.options_show_help_tooltips, s.show_help_tooltips));

	// Confirmations
	const auto confirmations = make_section(tt.options_confirmations_title);
	confirmations->add(std::make_shared<ui::check_control>(frame, tt.options_confirm_del, s.confirm_deletions));
	confirmations->add(std::make_shared<ui::check_control>(frame, tt.options_confirm_rotate, s.confirm_rotations));

	// Playback
	const auto playback = make_section(tt.options_playback_title);
	playback->add(std::make_shared<ui::check_control>(frame, tt.command_autoplay, s.auto_play));
	playback->add(std::make_shared<ui::check_control>(frame, tt.command_auto_advance, s.auto_advance));
	playback->add(std::make_shared<ui::check_control>(frame, tt.options_last_played_pos, s.last_played_pos));
	playback->add(std::make_shared<ui::check_control>(frame, tt.command_repeat_none, edit->repeat_none, true, false,
	                                                  nullptr, radio_group_repeat));
	playback->add(std::make_shared<ui::check_control>(frame, tt.command_repeat_one, edit->repeat_one, true, false,
	                                                  nullptr, radio_group_repeat));
	playback->add(std::make_shared<ui::check_control>(frame, tt.command_repeat_all, edit->repeat_all, true, false,
	                                                  nullptr, radio_group_repeat));
	playback->add(std::make_shared<text_element>(tt.audio_output_title));
	playback->add(make_chooser(frame, edit->sound_device_name, [edit](const std::shared_ptr<ui::select_control>& sel)
	{
		std::vector<ui::command_ptr> result;
		auto devices = platform::list_audio_playback_devices();
		devices.insert(devices.begin(), platform::sound_device{{}, std::string(tt.audio_device_default.sv())});

		for (const auto& d : devices)
		{
			auto c = std::make_shared<ui::command>();
			c->text = d.name;
			c->checked = edit->settings.sound_device == d.id;
			c->invoke = [edit, sel, d]
			{
				edit->settings.sound_device = d.id;
				sel->update_text(d.name);
			};
			result.emplace_back(std::move(c));
		}

		return result;
	}));
	playback->add(std::make_shared<text_element>(tt.option_slideshow_delay));
	playback->add(std::make_shared<ui::slider_control>(frame, std::string_view{}, s.slideshow_delay,
	                                                   settings_t::min_slideshow_delay,
	                                                   settings_t::max_slideshow_delay));

	// Subtitles
	const auto subtitles = make_section(tt.subtitles_title);
	subtitles->add(std::make_shared<ui::check_control>(frame, tt.subtitle_mode_foreign, edit->subtitles_foreign, true,
	                                                   false, nullptr, radio_group_subtitles));
	subtitles->add(std::make_shared<ui::check_control>(frame, tt.subtitle_mode_always, edit->subtitles_always, true,
	                                                   false, nullptr, radio_group_subtitles));
	subtitles->add(std::make_shared<ui::check_control>(frame, tt.subtitle_mode_off, edit->subtitles_off, true, false,
	                                                   nullptr, radio_group_subtitles));
	subtitles->add(std::make_shared<text_element>(tt.subtitle_language_label));
	subtitles->add(make_chooser(frame, edit->subtitle_language_name,
	                            [edit](const std::shared_ptr<ui::select_control>& sel)
	                            {
		                            std::vector<ui::command_ptr> result;
		                            std::vector<std::string_view> codes = {{}};
		                            codes.insert(codes.end(), std::begin(subtitle_languages),
		                                         std::end(subtitle_languages));

		                            for (const auto code : codes)
		                            {
			                            auto c = std::make_shared<ui::command>();
			                            c->text = subtitle_language_name(code);
			                            c->checked = edit->settings.subtitle_language == code;
			                            c->invoke = [edit, sel, code = std::string(code)]
			                            {
				                            edit->settings.subtitle_language = code;
				                            sel->update_text(subtitle_language_name(code));
			                            };
			                            result.emplace_back(std::move(c));
		                            }

		                            return result;
	                            }));

	// Collection folders
	const auto folders = make_section(tt.collection_options_local_folders_title);
	folders->add(std::make_shared<text_element>(tt.collection_info));
	folders->add(std::make_shared<link_element>(tt.more_collection_options_information, [] { help(); }));
	folders->add(std::make_shared<ui::check_control>(frame, local.pictures.text(), s.collection.pictures));
	folders->add(std::make_shared<ui::check_control>(frame, local.video.text(), s.collection.video));
	folders->add(std::make_shared<ui::check_control>(frame, local.music.text(), s.collection.music));

	if (local.onedrive_pictures.exists())
		folders->add(std::make_shared<ui::check_control>(frame, local.onedrive_pictures.text(),
		                                                 s.collection.onedrive_pictures));
	if (local.onedrive_video.exists())
		folders->add(std::make_shared<ui::check_control>(frame, local.onedrive_video.text(),
		                                                 s.collection.onedrive_video));
	if (local.onedrive_music.exists())
		folders->add(std::make_shared<ui::check_control>(frame, local.onedrive_music.text(),
		                                                 s.collection.onedrive_music));
	if (local.dropbox_photos.exists())
		folders->add(std::make_shared<ui::check_control>(frame, local.dropbox_photos.text(), s.collection.drop_box));

	// Other folders
	const auto other_folders = make_section(tt.collection_options_custom_folders_title);
	other_folders->add(std::make_shared<text_element>(tt.collection_options_more_folders));
	other_folders->add(std::make_shared<ui::folder_picker_control>(frame, edit->more_folders, true));
	other_folders->add(std::make_shared<text_element>(tt.collection_options_custom_locations_help));
	other_folders->add(std::make_shared<text_element>(tt.collection_options_custom_folders_help));

	// What each folder holds
	const auto library = make_section(tt.options_library_types_title);
	library->add(std::make_shared<text_element>(tt.options_library_types_help));

	if (edit->library_types.empty())
	{
		library->add(std::make_shared<text_element>(tt.options_library_types_none));
	}

	for (auto i = 0_z; i < edit->library_types.size(); ++i)
	{
		library->add(std::make_shared<text_element>(edit->library_types[i].first.text()));
		library->add(make_chooser(frame, edit->library_type_names[i],
		                          [edit, i](const std::shared_ptr<ui::select_control>& sel)
		                          {
			                          std::vector<ui::command_ptr> result;

			                          for (const auto kind : all_library_kinds)
			                          {
				                          auto c = std::make_shared<ui::command>();
				                          c->text = library_kind_name(kind);
				                          c->checked = edit->library_types[i].second == kind;
				                          c->invoke = [edit, sel, i, kind]
				                          {
					                          edit->library_types[i].second = kind;
					                          sel->update_text(library_kind_name(kind));
				                          };
				                          result.emplace_back(std::move(c));
			                          }

			                          return result;
		                          }));
	}

	if (!edit->library_types.empty())
	{
		library->add(std::make_shared<text_element>(tt.options_library_types_added));
	}

	library->add(std::make_shared<ui::check_control>(frame, tt.options_read_nfo, s.read_nfo));

	// Maintenance
	const auto maintenance = make_section(tt.index_maintenance_title);
	maintenance->add(std::make_shared<text_element>(format_index_text(_state)));
	maintenance->add(std::make_shared<text_element>(tt.index_maintenance_help));
	maintenance->add(std::make_shared<link_element>(tt.defragment_and_compact, [actions = _actions]
	{
		if (actions.index_maintenance) actions.index_maintenance();
	}));

	// Sidebar
	const auto sidebar = make_section(tt.options_sidebar_shown_title);
	sidebar->add(std::make_shared<ui::check_control>(frame, tt.command_nav_bar, s.show_sidebar));
	sidebar->add(std::make_shared<ui::check_control>(frame, tt.customize_show_total, s.sidebar.show_total_items));
	sidebar->add(std::make_shared<ui::check_control>(frame, tt.customize_show_history, s.sidebar.show_history));
	sidebar->add(std::make_shared<ui::check_control>(frame, tt.customize_show_world_map, s.sidebar.show_world_map));
	sidebar->add(std::make_shared<ui::check_control>(frame, tt.customize_show_drives, s.sidebar.show_drives));
	sidebar->add(std::make_shared<ui::check_control>(frame, tt.customize_show_searches,
	                                                 s.sidebar.show_favorite_searches));
	sidebar->add(std::make_shared<ui::check_control>(frame, tt.customize_show_tags, s.sidebar.show_tags));
	sidebar->add(std::make_shared<ui::check_control>(frame, tt.option_favorite_tags,
	                                                 s.sidebar.show_favorite_tags_only));
	sidebar->add(std::make_shared<ui::check_control>(frame, tt.customize_ratings, s.sidebar.show_ratings));
	sidebar->add(std::make_shared<ui::check_control>(frame, tt.customize_labels, s.sidebar.show_labels));
	sidebar->add(std::make_shared<text_element>(tt.customize_history_start_year));
	sidebar->add(std::make_shared<ui::num_control>(frame, std::string_view{}, s.sidebar.history_start_year, true));

	// Favorite searches and tags
	const auto searches = make_section(tt.customise_searches_title);
	searches->add(std::make_shared<ui::two_col_table_control>(frame, s.search.title, s.search.path,
	                                                          settings_t::search_t::count));

	const auto tags = make_section(tt.customise_tags_title);
	tags->add(std::make_shared<text_element>(tt.customise_tags_help));
	tags->add(std::make_shared<text_element>(tt.help_tag1));
	tags->add(std::make_shared<text_element>(tt.help_tag2));
	tags->add(std::make_shared<ui::multi_line_edit_control>(frame, s.favorite_tags));

	// Saving
	const auto saving = make_section(tt.options_save_options);
	saving->add(std::make_shared<ui::check_control>(frame, tt.options_backup_copy, s.create_originals));
	saving->add(std::make_shared<text_element>(tt.options_jpeg_quality));
	saving->add(std::make_shared<ui::slider_control>(frame, std::string_view{}, s.jpeg_save_quality, 0, 100));
	saving->add(std::make_shared<text_element>(tt.options_webp_quality));
	saving->add(std::make_shared<ui::slider_control>(frame, std::string_view{}, s.webp_quality, 1, 100));
	saving->add(std::make_shared<ui::check_control>(frame, tt.lossless_compression, s.webp_lossless));
	saving->add(std::make_shared<text_element>(tt.options_jxl_quality));
	saving->add(std::make_shared<ui::slider_control>(frame, std::string_view{}, s.jxl_quality, 1, 100));
	saving->add(std::make_shared<ui::check_control>(frame, tt.lossless_compression, s.jxl_lossless));

	// Advanced
	const auto advanced = make_section(tt.options_advanced);
#ifndef WINSTORE
	advanced->add(std::make_shared<ui::check_control>(frame, tt.options_check_for_update, s.check_for_updates));
	advanced->add(std::make_shared<ui::check_control>(frame, tt.options_send_crash_reports, s.send_crash_dumps));
#endif
	advanced->add(std::make_shared<ui::check_control>(frame, tt.options_use_gpu, s.use_gpu));
	advanced->add(std::make_shared<ui::check_control>(frame, tt.options_use_gpu_video, s.use_d3d11va));
	advanced->add(std::make_shared<ui::check_control>(frame, tt.options_show_debug_info, s.show_debug_info));

	// The page is in the sections a user looks for, each under a shaded header that names it and says what it
	// covers, with its cards in columns below. The collection comes first: it is the first thing to set up.
	const auto add_section = [&result](const icon_index icon, const std::string_view title,
	                                   const std::string_view info,
	                                   const std::initializer_list<std::shared_ptr<ui::group_control>> cards)
	{
		auto header = std::make_shared<options_section_header>(icon, title, info);
		const auto columns = std::make_shared<options_columns>();
		for (const auto& card : cards) columns->add(card);
		result->_controls.emplace_back(header);
		result->_controls.emplace_back(set_margin(columns, 8, 8));
		return header;
	};

	add_section(icon_index::set, tt.collection_title, tt.collection_options_info,
	            {folders, other_folders, library, maintenance});
	add_section(icon_index::preview, tt.options_section_viewing, tt.options_section_viewing_info,
	            {viewing, playback, subtitles});
	const auto sidebar_header = add_section(icon_index::navigation, tt.customise_sidebar_title,
	                                        tt.customise_sidebar_desc, {sidebar, searches, tags});
	add_section(icon_index::edit, tt.options_section_editing, tt.options_section_editing_info, {confirmations, saving});
	add_section(icon_index::settings, tt.options_section_general, tt.options_section_general_info,
	            {application, advanced});

	// Opened from the sidebar's own link, the page starts at the Sidebar section; otherwise at the top.
	if (_section == options_section::sidebar)
	{
		result->initial_focus = [weak = std::weak_ptr<view_controls_host>(result), start = sidebar_header]
		{
			if (const auto host = weak.lock()) host->_scroller.scroll_offset(host, 0, std::max(0, start->bounds.top - 8));
		};
	}

	return result;
}
