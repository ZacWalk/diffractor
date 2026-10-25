// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Localization and internationalization support. Loads language files (PO format)
// and provides text translation services for the entire application.

#include "pch.h"
#include "app_text.h"
#include "av_format.h"
#include "model_items.h"
#include "model_property.h"

// Declared in app_text.h and model_items.h respectively. Both formatters select wording from the
// active language, so they belong with the rest of the text layer rather than in the application
// frame and the edit view, where they used to be defined.

std::string format_plural_text(const plural_text& fmt, const std::string_view first_name, const int64_t count,
                               const df::file_size size, const int64_t of_total)
{
	// Select the plural form for the active language. Form 0 is the singular
	// (fmt.one), form 1 the general plural (fmt.plural), forms >= 2 the extra
	// Slavic forms. Missing extra forms fall back to the general plural.
	const int form = tt.plural_form(count);
	std::string_view template_text;

	if (form <= 0)
	{
		template_text = fmt.one;
	}
	else if (form == 1)
	{
		template_text = fmt.plural;
	}
	else
	{
		const auto extra = fmt.extra_form(static_cast<size_t>(form) - 2);
		template_text = extra.empty() ? std::string_view(fmt.plural) : extra;
	}

	const auto number = [](const int64_t n) { return platform::format_number(str::to_string(n)); };

	auto substitute = [&](std::ostringstream& result, const std::string_view token)
	{
		if (token == "first-name") result << first_name;
		else if (token == "count" || token.empty()) result << number(count);
		else if (token == "other") result << number(count - 1);
		else if (token == "total") result << number(of_total);
		else if (token == "size") result << df::format_size(size);
	};

	return str::replace_tokens(template_text, substitute);
}

std::string format_plural_text(const plural_text& fmt, const int64_t count, const int64_t of_total)
{
	return format_plural_text(fmt, {}, count, {}, of_total);
}

std::string format_plural_text(const plural_text& fmt, const df::item_set& items)
{
	const auto summary = items.summary();
	const auto total_items = summary.total_items() + summary.total_folders();
	return format_plural_text(fmt, items.first_name(), total_items.count, total_items.size, 0);
}

std::string format_plural_text(const plural_text& fmt, const std::vector<std::string>& result)
{
	const std::string_view first_name = result.empty() ? std::string_view{} : std::string_view(result.front());
	return format_plural_text(fmt, first_name, static_cast<int64_t>(result.size()), {}, 0);
}

std::string format_invalid_name_message(const std::string_view name)
{
	const auto name_error = str_format(tt.error_invalid_path_fmt.sv(), name);
	return std::format("{}\n{} \\ / : * ? \" < > |", name_error, tt.error_invalid_path);
}

std::string language_name(const std::string_view code)
{
	static const df::hash_map<std::string_view, std::string_view> code_to_name = {
		{"en", "English"},
		{"eng", "English"},
		{"de", "Deutsch (German)"},
		{"deu", "Deutsch (German)"},
		{"ger", "Deutsch (German)"},
		{"br", "brezhoneg (Breton)"},
		{"bre", "brezhoneg (Breton)"},
		{"cs", "čeština (Czech)"},
		{"ces", "čeština (Czech)"},
		{"cze", "čeština (Czech)"},
		{"es", "Español (Spanish)"},
		{"spa", "Español (Spanish)"},
		{"fr", "Français (French)"},
		{"fra", "Français (French)"},
		{"fre", "Français (French)"},
		{"it", "Italiano (Italian)"},
		{"ita", "Italiano (Italian)"},
		{"ja", "日本語 (Japanese)"},
		{"jpn", "日本語 (Japanese)"},
		{"ko", "한국어 (Korean)"},
		{"kor", "한국어 (Korean)"},
		{"lv", "Latviešu Valoda (Latvian)"},
		{"lav", "Latviešu Valoda (Latvian)"},
		{"nl", "Nederlands (Dutch)"},
		{"nld", "Nederlands (Dutch)"},
		{"dut", "Nederlands (Dutch)"},
		{"pl", "Polszczyzna (Polish)"},
		{"pol", "Polszczyzna (Polish)"},
		{"pt", "Português (Portuguese)"},
		{"por", "Português (Portuguese)"},
		{"ru", "Русский (Russian)"},
		{"rus", "Русский (Russian)"},
		{"sr", "српски језик (Serbian)"},
		{"srp", "српски језик (Serbian)"},
		{"tr", "Türkçe (Turkish)"},
		{"tur", "Türkçe (Turkish)"},
		{"uk", "Українська (Ukrainian)"},
		{"ukr", "Українська (Ukrainian)"},
		{"zh", "中文 (Chinese)"},
		{"zho", "中文 (Chinese)"},
		{"chi", "中文 (Chinese)"},
	};

	const auto found = code_to_name.find(code);
	return found == code_to_name.end() ? std::string(code) : std::string(found->second);
}

std::string format_audio_stream_name(const av_stream_info& stream, const int audio_track_number)
{
	std::vector<std::string> parts;

	if (!stream.title.empty()) parts.emplace_back(stream.title);
	if (!stream.language.empty() && stream.language != "und") parts.emplace_back(language_name(stream.language));
	if (stream.is_commentary) parts.emplace_back(tt.audio_role_commentary.sv());
	if (stream.is_audio_description) parts.emplace_back(tt.audio_role_description.sv());

	if (parts.empty())
	{
		parts.emplace_back(str_format(tt.audio_track_fmt.sv(), audio_track_number));
	}

	if (stream.audio_channels > 0)
	{
		parts.emplace_back(prop::format_audio_channels(stream.audio_channels));
	}
	else if (!stream.codec.empty())
	{
		parts.emplace_back(stream.codec);
	}

	std::string result;
	for (const auto& part : parts)
	{
		if (!result.empty()) result += " - ";
		result += part;
	}
	return result;
}

static std::string un_escape(const std::string_view text)
{
	const auto start = text.find(u8'"');
	const auto end = text.rfind(u8'"');

	if (start == std::string_view::npos) return {};
	if (end == std::string_view::npos) return {};
	if (start >= end) return {};

	const auto body = text.substr(start + 1, end - start - 1);

	std::string result;
	result.reserve(body.size());

	// Single left-to-right pass so an escaped backslash (\\) is not re-interpreted
	// together with the following character. A sequential replace of "\\n" -> newline
	// before "\\\\" -> "\\" would corrupt strings such as "\\\\nas" (an escaped \\nas).
	for (size_t i = 0; i < body.size(); ++i)
	{
		if (body[i] == '\\' && i + 1 < body.size())
		{
			switch (body[i + 1])
			{
			case 'n': result += '\n';
				++i;
				break;
			case 't': result += '\t';
				++i;
				break;
			case 'r': result += '\r';
				++i;
				break;
			case '"': result += '"';
				++i;
				break;
			case '\\': result += '\\';
				++i;
				break;
			default: result += body[i];
				break;
			}
		}
		else
		{
			result += body[i];
		}
	}

	return result;
}

std::string_view tt_prep(std::string_view result)
{
	const auto comment = result.find("//");

	if (comment != std::string::npos)
	{
		result = result.substr(0, comment);
	}

	return result;
}

static std::optional<int> parse_msgstr_plural_index(const std::string& line, std::vector<po_parse_error>& errors,
                                                    const size_t line_number)
{
	const auto open = line.find('[');
	const auto close = line.find(']', open == std::string::npos ? 0 : open + 1);

	if (open == std::string::npos || close == std::string::npos || close == open + 1)
	{
		errors.emplace_back(line_number, "Malformed msgstr plural index");
		return {};
	}

	const auto index_text = std::string_view(line).substr(open + 1, close - open - 1);

	if (!std::ranges::all_of(index_text, [](const char ch) { return ch >= '0' && ch <= '9'; }))
	{
		errors.emplace_back(line_number, "Malformed msgstr plural index");
		return {};
	}

	int form = -1;
	const auto* const first = index_text.data();
	const auto* const last = first + index_text.size();
	const auto parsed = std::from_chars(first, last, form);

	if (parsed.ec != std::errc{} || parsed.ptr != last)
	{
		errors.emplace_back(line_number, "Malformed msgstr plural index");
		return {};
	}

	if (static_cast<size_t>(form) > max_supported_plural_index)
	{
		errors.emplace_back(line_number, std::format("Unsupported msgstr plural index {}; Diffractor supports 0..{}",
		                                             form, max_supported_plural_index));
		return {};
	}

	auto tail = std::string_view(line).substr(close + 1);
	tail = str::trim(tail);
	if (!tail.empty() && tail.front() != '"')
	{
		errors.emplace_back(line_number, "Malformed msgstr plural index");
		return {};
	}

	return form;
}

po_load_result load_po_report(const df::file_path lang_file)
{
	po_load_result result;
	std::ifstream fs(platform::to_stream_path(lang_file));

	enum class parse_po_state
	{
		none,
		id,
		str1,
		str,
		id_plural,
		extra,
		ignore,
	};

	auto parse_state = parse_po_state::none;
	int extra_index = 0;
	po_entry entry;
	size_t line_number = 0;

	while (fs)
	{
		std::string line;
		std::getline(fs, line);
		++line_number;

		std::string::size_type pos = line.find_last_not_of(" \t\r\n");
		if (pos != std::string::npos && pos < line.size() - 1)
		{
			line.erase(pos + 1, std::string::npos);
		}

		if (!line.empty() && line[0] != '#')
		{
			try
			{
				if (str::starts(line, "msgstr["))
				{
					// Parse the form index N from msgstr[N]. Forms 0 and 1 map to
					// str/str_plural; forms >= 2 (Slavic "many" etc.) go to str_extra.
					const auto parsed_form = parse_msgstr_plural_index(line, result.errors, line_number);
					const auto form = parsed_form.value_or(-1);

					if (form == 0) parse_state = parse_po_state::str;
					else if (form == 1) parse_state = parse_po_state::str1;
					else if (form >= 2)
					{
						parse_state = parse_po_state::extra;
						extra_index = form - 2;
						if (static_cast<int>(entry.str_extra.size()) <= extra_index)
						{
							entry.str_extra.resize(extra_index + 1);
						}
					}
					else parse_state = parse_po_state::ignore;
				}
				else if (str::starts(line, "msgid_plural")) parse_state = parse_po_state::id_plural;
				else if (str::starts(line, "msgstr")) parse_state = parse_po_state::str;
				else if (str::starts(line, "msgid")) parse_state = parse_po_state::id;

				if (parse_state == parse_po_state::id && !entry.is_empty() && line[0] != u8'\"')
				{
					result.entries.emplace_back(std::move(entry));
					entry.clear();
				}

				auto value = un_escape(line);

				if (parse_state == parse_po_state::str1) entry.str_plural += value;
				else if (parse_state == parse_po_state::str) entry.str += value;
				else if (parse_state == parse_po_state::extra) entry.str_extra[extra_index] += value;
				else if (parse_state == parse_po_state::id_plural) entry.id_plural += value;
				else if (parse_state == parse_po_state::id) entry.id += value;
			}
			catch (std::invalid_argument&)
			{
				result.errors.emplace_back(line_number, "Malformed PO entry");
				parse_state = parse_po_state::ignore;
			}
		}
	}

	if (!entry.is_empty())
	{
		result.entries.emplace_back(std::move(entry));
	}

	return result;
}

std::vector<po_entry> load_po(const df::file_path lang_file)
{
	auto result = load_po_report(lang_file);
	for (const auto& error : result.errors)
	{
		df::log(__FUNCTION__, std::format("{}:{}: {}", lang_file.name(), error.line, error.message));
	}
	return std::move(result.entries);
}


namespace
{
	// Lightweight plural-form rule table. Each function maps a count to a
	// gettext-style form index, matching the Plural-Forms header in the .po
	// files. Only languages whose grammar needs more than a binary one/plural
	// split are listed here; every other language uses plural_binary and behaves
	// exactly as before this table existed.
	//
	// NOTE (Slavic form 0): CLDR form 0 also covers 21, 31, 101 ... in Russian
	// and Ukrainian. Some singular sources use a literal "1" (e.g. "1 folder"),
	// whose translation msgstr[0] is only correct for count == 1. To avoid
	// rendering "1 folder" for 21 items, app_text_t::plural_form re-maps form 0
	// to form 1 whenever count != 1. Czech and Polish put only n == 1 in form 0,
	// so that clamp never affects them and they keep the full CLDR distinction.

	int plural_binary(const int64_t n) { return n == 1 ? 0 : 1; }

	int plural_cs(int64_t n)
	{
		if (n < 0) n = -n;
		if (n == 1) return 0;
		if (n >= 2 && n <= 4) return 1;
		return 2;
	}

	int plural_pl(int64_t n)
	{
		if (n < 0) n = -n;
		if (n == 1) return 0;
		if (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14)) return 1;
		return 2;
	}

	// Russian and Ukrainian share the same three integer forms (Ukrainian's
	// fourth CLDR form only applies to fractions, which counts never are).
	int plural_ru_uk(int64_t n)
	{
		if (n < 0) n = -n;
		if (n % 10 == 1 && n % 100 != 11) return 0;
		if (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14)) return 1;
		return 2;
	}

	using plural_rule_fn = int(*)(int64_t);

	plural_rule_fn plural_rule_for(const std::string_view code)
	{
		if (code == "cs") return plural_cs;
		if (code == "pl") return plural_pl;
		if (code == "ru" || code == "uk") return plural_ru_uk;
		return plural_binary;
	}
}

int app_text_t::plural_form(const int64_t count) const
{
	const auto rule = _plural_rule ? _plural_rule : plural_binary;
	int idx = rule(count);
	// Never reuse the singular (msgstr[0]) form for counts other than 1; see the
	// rule-table note above.
	if (idx == 0 && count != 1) idx = 1;
	return idx;
}


app_text_t::app_text_t()
{
	// The platform layer's defaults name no application. This is Diffractor's wording, and the
	// msgids its catalogs translate.
	platform_text.error_sse2_needed = "Diffractor requires a processor with sse2.";
	platform_text.error_unsupported_os = "Diffractor requires Windows 7 or newer.";
	platform_text.error_cannot_continue = "Diffractor has a serious problem and cannot continue.";
	platform_text.title_error = "Diffractor ERROR";
	calc_text_mapping();
}

void app_text_t::load_lang(const std::string_view lang_file, const std::vector<po_entry>& entries)
{
	// default
	clear();

	// Select the plural rule from the language code (the file name without the
	// ".po" extension). Unlisted languages fall back to the binary rule.
	auto code = lang_file;
	const auto dot = code.find('.');
	if (dot != std::string_view::npos) code = code.substr(0, dot);
	_plural_rule = plural_rule_for(code);

	df::hash_map<std::string_view, std::string_view> text_map;
	df::hash_map<std::string_view, const std::vector<std::string>*> extra_map;

	for (const auto& entry : entries)
	{
		if (!entry.id.empty() && !entry.str.empty())
		{
			text_map[entry.id] = entry.str;
		}

		if (!entry.id_plural.empty() && !entry.str_plural.empty())
		{
			text_map[entry.id_plural] = entry.str_plural;
		}

		if (!entry.id_plural.empty() && !entry.str_extra.empty())
		{
			extra_map[entry.id_plural] = &entry.str_extra;
		}
	}


	// A partly-translated catalog is missing hundreds of strings, which would otherwise be one log
	// line each on every launch. The count is what a bug report needs; app_validate_po lists them.
	auto missing = 0;
	auto total = 0;

	// Storage that outlives the switch, so a worker holding a translation pointer keeps valid text.
	const auto publish = [this](text_t& target, const std::string_view translated)
	{
		target.publish(&_translation_storage.emplace_back(translated));
	};

	for (const auto& t : _all_texts)
	{
		auto found = text_map.find(t.get().text);
		++total;

		if (found != text_map.end())
		{
			publish(t.get(), found->second);
		}
		else
		{
			++missing;
			df::trace(std::format("{} missing: msgid \"{}\"", lang_file, t.get().text));
			t.get().clear();
		}
	}

	for (const auto& p : _all_plurals)
	{
		auto found = text_map.find(p.get().one.text);
		total += 2;

		if (found != text_map.end())
		{
			publish(p.get().one, found->second);
		}
		else
		{
			++missing;
			df::trace(std::format("{} missing: msgid \"{}\"", lang_file, p.get().one.text));
			p.get().one.clear();
		}

		found = text_map.find(p.get().plural.text);

		if (found != text_map.end())
		{
			publish(p.get().plural, found->second);
		}
		else
		{
			++missing;
			df::trace(std::format("{} missing: msgid_plural \"{}\"", lang_file, p.get().plural.text));
			p.get().plural.clear();
		}

		// Extra Slavic plural forms (msgstr[2..]); none for binary languages.
		const auto found_extra = extra_map.find(p.get().plural.text);
		p.get().extra_forms.store(found_extra != extra_map.end()
			                          ? &_plural_form_storage.emplace_back(*found_extra->second)
			                          : nullptr,
		                          std::memory_order_release);
	}

	df::log(__FUNCTION__, std::format("{}: {} of {} strings translated", lang_file, total - missing, total));
}

void app_text_t::clear()
{
	for (auto&& t : _all_texts)
	{
		t.get().clear();
	}

	for (auto&& p : _all_plurals)
	{
		p.get().one.clear();
		p.get().plural.clear();
		p.get().extra_forms.store(nullptr, std::memory_order_release);
	}

	_plural_rule = nullptr;
}

void app_text_t::calc_text_mapping()
{
	_all_texts = std::vector<std::reference_wrapper<text_t>>
	{
		about_info,
		add_folder,
		adjust_date_help1,
		adjust_date_help2,
		adjust_date_help3,
		adjust_date_required,
		after,
		album_artist,
		analyzing,
		archive,
		archives,
		aspect_ratio_other_landscape,
		aspect_ratio_other_portrait,
		audio,
		audio_output_title,
		audio_role_commentary,
		audio_role_description,
		audio_track_current_fmt,
		audio_track_fmt,
		audio_tracks_title,
		audio_title,
		before,
		brightness,
		burn_failed,
		burn_help,
		burn_stage,
		burn_title,
		analyze,
		button_analyze,
		button_cancel,
		button_change,
		button_close,
		button_convert,
		button_delete,
		button_dont_save,
		button_ok,
		button_rotate,
		button_save,
		button_save_as_jpeg,
		button_send,
		button_share,
		button_sync,
		button_tag,
		button_update,
		donate,
		donate_help,
		donate_link,
		diagnostics_sent,
		diagnostics_send_failed,
		cancel_was_pressed_after,
		cannot_edit_fmt,
		changes,
		map_instructions,
		collection_contains,
		collection_contains2,
		items_created_fmt,
		items_modified_fmt,
		open_created_modified,
		click_items_from_fmt,
		map_items_close_to_fmt,
		filter,
		close,
		collection_options_custom_folders_help,
		collection_options_custom_folders_title,
		collection_options_custom_locations_help,
		collection_options_info,
		collection_options_local_folders_title,
		collection_options_more_folders,
		more_collection_options_information,
		more_template_information,
		collection_title,
		color,
		command_adjust_date,
		command_advanced_search,
		command_app_exit,
		command_browse_back,
		command_browse_forward,
		command_browse_next_folder,
		command_browse_next_group,
		command_browse_next_item,
		command_browse_next_item_extend,
		command_browse_parent,
		command_browse_previous_folder,
		command_browse_previous_group,
		command_browse_previous_item,
		command_browse_previous_item_extend,
		command_burn,
		command_auto_color,
		command_auto_document,
		tooltip_auto_document,
		command_auto_straighten,
		edit_original,
		edit_info,
		movie_title,
		movie_info,
		movie_settings_title,
		movie_clip_title,
		movie_crossfade,
		movie_cut,
		movie_transition_length,
		movie_fade_length,
		movie_fade_in,
		movie_fade_out,
		movie_photo_duration,
		movie_clip_duration,
		movie_use_default,
		movie_empty,
		movie_output_fmt,
		movie_clip_fmt,
		movie_project_files,
		movie_maker_files,
		movie_media_files,
		movie_open_failed_fmt,
		movie_save_failed_fmt,
		movie_video_files,
		movie_rendering,
		movie_render_failed_fmt,
		movie_error_no_length,
		movie_error_no_encoder,
		movie_error_no_memory,
		movie_error_undecodable,
		movie_error_encoder_failed,
		movie_error_output_is_source,
		movie_rendered_fmt,
		movie_render_cancelled,
		movie_relink_none,
		movie_unsaved_prompt,
		movie_discard,
		command_movie,
		tooltip_movie,
		command_movie_render,
		tooltip_movie_render,
		command_movie_relink,
		tooltip_movie_relink,
		command_movie_add,
		command_movie_remove,
		command_movie_send_to_end,
		command_movie_open,
		command_movie_save,
		command_movie_import,
		convert_info,
		edit_metadata_info,
		adjust_date_info,
		tag_info,
		command_edit_preview,
		command_capture,
		command_close,
		command_cancel,
		command_collection_options,
		command_color_reset,
		command_convert_or_resize,
		command_convert,
		convert_preparing,
		command_copy,
		command_customise,
		command_delete,
		command_desktop_background,
		command_display_options,
		command_edit,
		command_edit_copy,
		command_edit_copy_item_path,
		command_edit_cut,
		command_edit_metadata,
		command_update_metadata,
		command_edit_paste,
		command_eject,
		command_favorite,
		command_file_properties,
		command_file_search,
		command_flatten,
		command_fullscreen,
		command_group_album,
		command_group_aspect_ratio,
		command_group_camera,
		command_group_created,
		command_group_original,
		command_group_extension,
		command_group_file_type,
		command_group_location,
		command_group_modified,
		command_group_resolution,
		command_group_presence,
		command_group_rating,
		command_group_shuffle,
		command_group_size,
		command_group_folder,
		command_highlight_large_items,
		command_import,
		import_preparing,
		command_keyboard,
		command_label_approved,
		command_label_none,
		command_label_review,
		command_label_second,
		command_label_select,
		command_label_to_do,
		command_language,
		command_locate,
		command_maximize,
		command_menu_group_sort,
		command_minimize,
		command_move,
		command_nav_bar,
		command_navigate,
		command_new_folder,
		command_new_version,
		command_check_for_updates,
		command_open,
		command_open_google_map,
		command_open_with,
		command_options,
		command_pin,
		command_play,
		command_print,
		command_rate,
		command_rate_0,
		command_rate_1,
		command_rate_2,
		command_rate_3,
		command_rate_4,
		command_rate_5,
		command_rate_rejected,
		command_refresh,
		command_related,
		command_rename,
		command_rename_files,
		command_repeat_toggle,
		command_restore,
		command_rotate_anticlockwise,
		command_rotate_clockwise,
		command_rotate_reset,
		command_save,
		command_save_and_back,
		command_save_and_back_tooltip,
		command_save_and_next,
		command_save_and_next_tooltip,
		command_locate_and_back,
		command_locate_and_next,
		command_save_as,
		tooltip_auto_color,
		tooltip_auto_straighten,
		tooltip_edit_preview,
		command_scale_up,
		command_scan,
		command_select_all,
		command_select_invert,
		command_select_nothing,
		tooltip_related,
		related_group_duplicates,
		related_group_album,
		related_group_series,
		related_group_time,
		related_group_location,
		command_share_email,
		email_preparing,
		command_share,
		share_preparing,
		command_show_in_file_browser,
		command_show_in_folder,
		command_sort_date_created,
		command_sort_date_original,
		command_sort_date_modified,
		command_sort_dates_ascending,
		command_sort_dates_descending,
		command_sort_def,
		command_sort_name,
		command_sort_size,
		command_sync,
		command_toggle_details,
		command_toggle_group_by,
		command_toggle_item_size,
		tooltip_thumbnail_size,
		tooltip_scroll_to_top,
		command_tools,
		command_view_help,
		command_view_items,
		command_view_large_font,
		command_view_menu,
		command_view_rate_label,
		command_view_select,
		command_view_sort,
		command_toggle_volume,
		command_zoom,
		command_zoom_fit,
		command_zoom_fit_width,
		command_zoom_fill,
		command_zoom_presets,
		command_zoom_in,
		command_zoom_flip,
		command_zoom_out,
		command_zoom_100,
		command_zoom_navigator,
		command_zoom_navigator_auto_hide,
		command_zoom_navigator_off,
		compare,
		compare_tooltip,
		contrast,
		open_link_fmt,
		open_link_choose,
		duplicate_text,
		copy_to_clipboard,
		copy_to_join,
		copyright_creator,
		copyright_credit,
		copyright_notice,
		copyright_source,
		copyright_title,
		copyright_url,
		count,
		customise_searches_title,
		customise_sidebar_title,
		customise_sidebar_desc,
		customise_tags_help,
		customise_tags_title,
		customize_history_start_year,
		customize_labels,
		customize_ratings,
		customize_show_drives,
		customize_show_history,
		customize_show_searches,
		customize_show_tags,
		customize_show_total,
		customize_show_world_map,
		darks,
		data,
		dates_file_created,
		dates_file_modified,
		dates_metadata_created,
		dates_title,
		default_favorite_tags,
		defragment_and_compact,
		defragmenting,
		delete_error,
		delete_fmt,
		delete_no_recycle_warning,
		desktop_background_info,
		destination_folder,
		disk_capacity,
		disk_free,
		disk_label,
		disk_system,
		disk_used,
		documentation,
		duplicates,
		duplicates_tooltip,
		editing_title,
		eject_close_info,
		eject_failed_fmt,
		eject_help,
		eject_none_found,
		eject_title_fmt,
		email_connecting_to_mapi,
		email_canceled,
		email_convert_to_jpeg,
		email_failed,
		email_limit_dimensions,
		email_processing_fmt,
		email_sending,
		email_small_help,
		email_zip,
		share_small_help,
		share_zip,
		share_failed,
		empty,
		ending_fmt,
		platform_text.error_access_denied_src,
		platform_text.error_atl_direct3d,
		platform_text.error_cannot_continue,
		platform_text.error_connect_scanner,
		error_create_file_failed_fmt,
		error_create_folder_failed_fmt,
		error_create_window_failed,
		platform_text.error_dest_is_cd_record,
		platform_text.error_dest_is_cd_rom,
		platform_text.error_dest_is_dvd,
		platform_text.error_dest_same_tree,
		platform_text.error_dest_subtree,
		platform_text.error_diff_dir,
		platform_text.error_dst_root_dir,
		platform_text.error_file_dest_is_fld,
		platform_text.error_file_too_large,
		platform_text.error_filename_too_long,
		platform_text.error_fld_dest_is_file,
		error_index_database_failed,
		platform_text.error_invalid_files,
		error_invalid_path_fmt,
		error_invalid_path,
		platform_text.error_many_dest,
		platform_text.error_many_src_1_dest,
		platform_text.error_max,
		platform_text.error_ole_failed,
		platform_text.error_on_dest,
		platform_text.error_op_cancelled,
		error_analysis_cancelled,
		platform_text.error_path_too_deep,
		error_rename_failed,
		platform_text.error_same_file,
		platform_text.error_save_image,
		platform_text.error_scanner,
		platform_text.error_src_is_cd_record,
		platform_text.error_src_is_cdrom,
		platform_text.error_src_is_dvd,
		platform_text.error_src_root_dir,
		platform_text.error_sse2_needed,
		platform_text.error_unknown,
		platform_text.error_unsupported_os,
		platform_text.error_windows_common_controls_failed,
		platform_text.error_winsock_failed,
		exif_metadata_title,
		failed_to_create_folder_fmt,
		favorite_add_fmt,
		favorite_failed_to_add,
		favorite_remove_fmt,
		favorite_title,
		favorite_info,
		collection_in,
		collection_not_in,
		collection_info,
		folder,
		folder_noun,
		folder_noun_plural,
		folder_title,
		folders,
		genre_a_capella,
		genre_abstract,
		genre_acid,
		genre_acidjazz,
		genre_acidpunk,
		genre_acoustic,
		genre_action,
		genre_action_adventure,
		genre_aerial,
		genre_alternative,
		genre_alternrock,
		genre_ambient,
		genre_analog,
		genre_animation,
		genre_anime,
		genre_architectural,
		genre_avantgarde,
		genre_aviation,
		genre_ballad,
		genre_bass,
		genre_bebob,
		genre_bigband,
		genre_bluegrass,
		genre_blues,
		genre_booty_bass,
		genre_brazilian,
		genre_cabaret,
		genre_candid,
		genre_celtic,
		genre_chambermusic,
		genre_chanson,
		genre_childrens,
		genre_chorus,
		genre_christian_gospel,
		genre_christian_rap,
		genre_classic,
		genre_classic_rock,
		genre_classical,
		genre_close_up,
		genre_cloudscape,
		genre_club,
		genre_comedy,
		genre_conceptual,
		genre_concert,
		genre_concert_films,
		genre_conservation,
		genre_country,
		genre_cult,
		genre_dance,
		genre_dance_hall,
		genre_darkwave,
		genre_death_metal,
		genre_disco,
		genre_documentary,
		genre_drama,
		genre_dream,
		genre_drum_solo,
		genre_duet,
		genre_easylistening,
		genre_electronic,
		genre_ethnic,
		genre_euro_dance,
		genre_euro_house,
		genre_euro_techno,
		genre_family,
		genre_fashion,
		genre_fast_fusion,
		genre_film_still,
		genre_fine_art,
		genre_fire,
		genre_fireworks,
		genre_fitness_workout,
		genre_folk,
		genre_folk_rock,
		genre_folklore,
		genre_food,
		genre_foreign,
		genre_forensic,
		genre_freestyle,
		genre_funk,
		genre_fusion,
		genre_game,
		genre_gangsta,
		genre_geophotography,
		genre_glamour,
		genre_gospel,
		genre_gothic,
		genre_gothic_rock,
		genre_grunge,
		genre_hardrock,
		genre_high_speed,
		genre_high_key,
		genre_hip_hop,
		genre_hip_hop_rap,
		genre_holiday,
		genre_horror,
		genre_house,
		genre_humour,
		genre_independent,
		genre_industrial,
		genre_instrumental,
		genre_instrumental_pop,
		genre_instrumental_rock,
		genre_jazz,
		genre_jazzfun,
		genre_jungle,
		genre_kids,
		genre_kids_family,
		genre_kirlian,
		genre_landscape,
		genre_latin,
		genre_lifestyle,
		genre_lo_fi,
		genre_lomography,
		genre_long_exposure,
		genre_low_key,
		genre_macro,
		genre_medical,
		genre_meditative,
		genre_metal,
		genre_monochrome,
		genre_music_documentaries,
		genre_music_feature_films,
		genre_musical,
		genre_musicals,
		genre_narrative,
		genre_national_folk,
		genre_native_american,
		genre_new_age,
		genre_new_wave,
		genre_night,
		genre_noise,
		genre_nonfiction,
		genre_oldies,
		genre_opera,
		genre_other,
		genre_panorama,
		genre_panoramic,
		genre_film_noir,
		genre_photo_op,
		genre_photobiography,
		genre_photojournalism,
		genre_photowalking,
		genre_podcast,
		genre_polaroid,
		genre_polka,
		genre_pop,
		genre_pop_folk,
		genre_pop_funk,
		genre_porn_groove,
		genre_portrait,
		genre_power_ballad,
		genre_pranks,
		genre_primus,
		genre_progressive_rock,
		genre_psychadelic,
		genre_psychedelic_rock,
		genre_punk,
		genre_punk_rock,
		genre_randb,
		genre_rap,
		genre_rave,
		genre_reality_tv,
		genre_reggae,
		genre_retro,
		genre_revival,
		genre_rhythmic_soul,
		genre_rock,
		genre_rock_and_roll,
		genre_romance,
		genre_samba,
		genre_satellite,
		genre_satire,
		genre_scifi_and_fantasy,
		genre_short_films,
		genre_show_tunes,
		genre_singer_songwriter,
		genre_ska,
		genre_slow_jam,
		genre_slow_rock,
		genre_social,
		genre_soft_focus,
		genre_sonata,
		genre_soul,
		genre_sound_clip,
		genre_soundtrack,
		genre_southern_rock,
		genre_space,
		genre_special_interest,
		genre_speech,
		genre_sports,
		genre_star_trail,
		genre_still_life,
		genre_stock,
		genre_street,
		genre_subminiature,
		genre_swing,
		genre_symphonic_rock,
		genre_symphony,
		genre_tango,
		genre_techno,
		genre_techno_industrial,
		genre_teens,
		genre_thriller,
		genre_time_lapse,
		genre_top40,
		genre_trailer,
		genre_trance,
		genre_travel,
		genre_tribal,
		genre_trip_hop,
		genre_ultraviolet,
		genre_underwater,
		genre_unknown,
		genre_urban,
		genre_vernacular,
		genre_vintage,
		genre_vocal,
		genre_war,
		genre_western,
		genre_world,
		group_sort_tooltip,
		group_title_items,
		group_title_no_album_or_show,
		group_title_no_aspect_ratio,
		group_title_no_camera,
		group_title_no_extension,
		group_title_no_location,
		group_title_no_resolution,
		group_title_no_rating,
		group_title_no_value,
		group_title_shuffle,
		group_title_size_range_fmt,
		group_title_today,
		group_title_yesterday,
		has_changes,
		help_artist,
		help_more_info,
		help_send_info,
		help_tag_add_remove,
		help_tag1,
		help_tag2,
		hide_verbose_metadata,
		history_navigator_action,
		image_display_failed,
		image_too_large,
		import_dest_folder,
		import_from,
		import_ignore_previous,
		import_info,
		import_other_folder,
		import_replace_warning,
		import_dest_folder_structure,
		import_set_created_date,
		index_maintenance_help,
		index_maintenance_reset_recommended,
		index_maintenance_title,
		index_size_fmt,
		indexing,
		indexing_message,
		invalid,
		iptc_metadata_title,
		is_not_valid_folder_fmt,
		item_oriented,
		item_oriented_tooltip_fmt,
		items_identical,
		items_not_identical,
		platform_text.jpeg_best,
		keyboard,
		keyboard_accelerator_press,
		keyboard_alt,
		keyboard_back,
		keyboard_backspace,
		keyboard_basics_title,
		keyboard_browser_back,
		keyboard_browser_favorites,
		keyboard_browser_forward,
		keyboard_browser_home,
		keyboard_browser_refresh,
		keyboard_browser_search,
		keyboard_browser_stop,
		keyboard_control,
		keyboard_ctrl_left_right_desc,
		keyboard_del,
		keyboard_down,
		keyboard_edit_title,
		keyboard_end,
		keyboard_enter,
		keyboard_enter_desc,
		keyboard_escape,
		keyboard_escape_desc,
		keyboard_f1,
		keyboard_f10,
		keyboard_f11,
		keyboard_f2,
		keyboard_f3,
		keyboard_f4,
		keyboard_f5,
		keyboard_f6,
		keyboard_f7,
		keyboard_f8,
		keyboard_f9,
		keyboard_file_management_title,
		keyboard_group_title,
		keyboard_help_title,
		keyboard_home,
		keyboard_home_end_desc,
		keyboard_insert,
		keyboard_left,
		keyboard_left_right_desc,
		keyboard_media_next_track,
		keyboard_media_play_pause,
		keyboard_media_prev_track,
		keyboard_media_stop,
		keyboard_navigation_title,
		keyboard_next,
		keyboard_oem_4,
		keyboard_oem_6,
		keyboard_oem_minus,
		keyboard_oem_plus,
		keyboard_open_title,
		options_title,
		keyboard_or,
		keyboard_playback_title,
		keyboard_prior,
		keyboard_rate_label_title,
		keyboard_ref_title,
		keyboard_right,
		keyboard_selection_title,
		keyboard_shift,
		keyboard_space,
		keyboard_space_desc,
		keyboard_tab,
		keyboard_tools_title,
		keyboard_up,
		keyboard_up_down_desc,
		keyboard_volume_down,
		keyboard_volume_mute,
		keyboard_volume_up,
		keyboard_zoom_keys_desc,
		km_from,
		learn_more_diffractor_com,
		lights,
		dimension_must_be_positive,
		limit_output_dimensions,
		lossless_compression,
		list_of_accelerators,
		loading,
		location_not_selected,
		location_overwrite_gps,
		location_title,
		maximize_image,
		media_metadata_title,
		metadata_title,
		metadata_select_field,
		genre_separator_help,
		platform_text.menu_add_fmt,
		platform_text.menu_copy,
		platform_text.menu_cut,
		platform_text.menu_delete,
		menu_move,
		platform_text.menu_paste,
		platform_text.menu_select_all,
		platform_text.menu_undo,
		metadata,
		midtones,
		words.month_april,
		words.month_august,
		words.month_december,
		words.month_february,
		words.month_january,
		words.month_july,
		words.month_june,
		words.month_march,
		words.month_may,
		words.month_november,
		words.month_october,
		words.month_september,
		words.month_short_apr,
		words.month_short_aug,
		words.month_short_dec,
		words.month_short_feb,
		words.month_short_jan,
		words.month_short_jul,
		words.month_short_jun,
		words.month_short_mar,
		words.month_short_may,
		words.month_short_nov,
		words.month_short_oct,
		words.month_short_sep,
		move_items,
		nav_history_title,
		nav_search_title,
		new_folder_name,
		new_folder_title,
		no_items_are_selected,
		none,
		not_supported_cloud,
		not_supported_folder,
		not_supported_photo,
		not_supported_photo_edit,
		not_supported_readonly,
		not_supported_readonly_metadata,
		not_supported_save_format,
		nothing_found1,
		nothing_found2,
		nothing_found_add_folders,
		empty_folder,
		num_of,
		open_dest,
		open_in_browser_title,
		open_properties_title,
		open_title,
		open_with_app,
		open_with_app_tool,
		open_with_failed,
		open_with_fmt,
		open_with_title,
		open_with_tool,
		option_slideshow_delay,
		option_slideshow_title,
		options_advanced,
		options_app_options,
		options_backup_copy,
		options_check_for_update,
		options_confirm_del,
		options_confirm_rotate,
		options_jpeg_quality,
		options_webp_quality,
		options_save_options,
		options_send_crash_reports,
		options_show_debug_info,
		options_show_hidden,
		options_show_rotated,
		options_updates,
		options_use_gpu,
		options_use_gpu_video,
		options_show_shadow,
		options_last_played_pos,
		options_show_help_tooltips,
		orientation_bottom_left,
		orientation_bottom_right,
		orientation_left_bottom,
		orientation_left_top,
		orientation_right_bottom,
		orientation_right_top,
		orientation_top_left,
		orientation_top_right,
		other,
		others,
		pasted_file_name,
		photo,
		photos,
		pixels_icon,
		pixels_identical_files_not_identical,
		resolution_none,
		pixels_small,
		pixels_title,
		platform_text.png_best,
		presence_loading,
		presence_newer_in,
		presence_newer_in_long,
		presence_not_in,
		presence_not_in_long,
		presence_older_in,
		presence_older_in_long,
		presence_similar_in,
		presence_similar_in_long,
		presence_this_in,
		presence_this_in_long,
		presence_tile,
		preview_rendered,
		preview_rendering,
		preview_show_preview,
		preview_showing,
		print_title,
		processing,
		processing_files,
		prop_name_35mmequivalent,
		prop_name_album,
		prop_name_albumartist,
		prop_name_artist,
		prop_name_audiocodec,
		prop_name_bitrate,
		prop_name_camera,
		prop_name_cameramanufacturer,
		prop_name_channels,
		prop_name_comment,
		prop_name_composer,
		prop_name_copyrightcreator,
		prop_name_copyrightcredit,
		prop_name_copyrightnotice,
		prop_name_copyrightsource,
		prop_name_copyrighturl,
		prop_name_country,
		prop_name_created,
		prop_name_createdexif,
		prop_name_original,
		prop_name_description,
		prop_name_digitized,
		prop_name_dimensions,
		prop_name_disk,
		prop_name_doc_id,
		prop_name_duration,
		prop_name_encoder,
		prop_name_encodingtool,
		prop_name_episode,
		prop_name_exposure,
		prop_name_filename,
		prop_name_fnumber,
		prop_name_focallength,
		prop_name_game,
		prop_name_genre,
		prop_name_id,
		prop_name_iso,
		prop_name_altitude,
		prop_name_gps_speed,
		height_in_flight,
		height_altitude_fmt,
		height_underwater_fmt,
		distance_label_fmt,
		tooltip_search_distance,
		compass_points,
		bearing_fmt,
		location_near_fmt,
		location_remote,
		nothing_within_fmt,
		nothing_at_place_fmt,
		widen_to_fmt,
		search_all_named_fmt,
		show_without_location,
		search_within_fmt,
		height_high_altitude,
		timeline_title,
		places_title,
		visit_range_fmt,
		visit_items_fmt,
		grouped_by_fmt,
		grouped_and_sorted_fmt,
		clear_timeline_selection,
		prop_name_label,
		prop_name_latitude,
		prop_name_lens,
		prop_name_longitude,
		prop_name_mediacategory,
		prop_name_megapixels,
		prop_name_modified,
		prop_name_null,
		prop_name_orientation,
		prop_name_performer,
		prop_name_pixelformat,
		prop_name_place,
		prop_name_publisher,
		prop_name_rating,
		prop_name_rawfile,
		prop_name_samplerate,
		prop_name_sampletype,
		prop_name_season,
		prop_name_show,
		prop_name_size,
		prop_name_state,
		prop_name_streams,
		prop_name_synopsis,
		prop_name_system,
		prop_name_tag,
		prop_name_title,
		prop_name_track,
		prop_name_videocodec,
		prop_name_year,
		query_age,
		query_and,
		query_created,
		query_duplicates,
		query_duplicates_alt1,
		query_duplicates_alt2,
		query_modified,
		query_or,
		query_original,
		query_related,
		query_with,
		query_without,

		rate_title,
		rating_keys,
		rating_remove_fmt,
		rating_replaces_reject,
		label_click_apply,
		label_click_clear,
		label_replaces_fmt,
		label_reject_replaces_rating,
		raw_metadata_title,
		remove_metadata_title,
		rename_label,
		rename_help_template_1,
		rename_help_template_2,
		rename_help_template_3,
		for_example,
		rename_help_template_example_2,
		rename_help_template_example_3,
		rename_help_template_example_4,
		rename_info,
		rename_template_label,
		rename_template_start_label,
		releases,
		repeat_help,
		repeat_off_help,
		repeat_one_help,
		reset_database,
		resetting,
		retro,
		retro_title,
		rotate_confirm_single,
		saturation,
		perspective_horizontal,
		perspective_vertical,
		save_as_jpeg_fmt,
		save_changes,
		save_new_photo,
		saving_file_name,
		scan_failed,
		search_all_terms,
		search_audio,
		search_christmas,
		search_collection,
		search_date_from,
		search_date_until,
		search_folder,
		search_last_7_days,
		search_located_within,
		search_none_terms,
		search_or_folder,
		search_photos,
		search_select_term,
		search_no_criteria,
		search_sub_folders,
		search_videos,
		searching_text,
		platform_text.select_folder,
		select_location,
		selected_date_range_label,
		selected_items_fmt,
		show_raw,
		show_raw_now,
		show_related,
		show_verbose_metadata,
		size_title,
		sort_by_album_show,
		sort_by_aspect_ratio,
		sort_by_def,
		sort_by_extension,
		sort_by_file_type,
		sort_by_location,
		sort_by_name,
		sort_by_Folder,
		sort_by_resolution,
		sort_by_presence,
		sort_by_rating_label,
		sort_by_shuffle,
		sort_by_size,
		staring,
		starting_date_label,
		starting_fmt,
		straighten,
		temperature,
		tint,
		stream_name_fmt,
		subtitle,
		support,
		sync_collection,
		sync_analysis_changed,
		sync_preparing,
		sync_copy_local_action,
		sync_copy_remote_action,
		sync_delete_count_fmt,
		sync_delete_local_action,
		sync_delete_remote_action,
		sync_delete_warning,
		sync_delete_local,
		sync_delete_remote,
		sync_info_1,
		sync_info_2,
		sync_local,
		sync_local_remote,
		sync_other_folder,
		sync_remote,
		sync_remote_local,
		tag_add_or_remove_label,
		tag_add_remove,
		tag_selected,
		tags_common_label,
		tags_favorite_label,
		tags_remove_label,
		tags_title,
		command_apply_tags,
		tags_column_item,
		tags_column_result,
		tags_nothing_to_do,
		tags_unchanged,
		tags_view_empty_message,
		command_cancel_operation,
		cancel_operation_title,
		cancel_operation_fmt,
		button_cancel_operation,
		button_keep_running,
		scope_unavailable_title,
		scope_unavailable_fmt,
		scope_unavailable_retry,
		scope_unavailable_parent,
		scope_busy_fmt,
		safe_start_title,
		safe_start_message,
		collision_policy_label,
		collision_replace,
		collision_skip,
		collision_rename,
		collision_block,
		collision_exists,
		collision_blocked_fmt,
		collision_skipped_fmt,
		collision_replaced_fmt,
		collision_renamed_fmt,
		result_success,
		result_failed,
		result_skipped,
		result_not_run,
		run_needs_refresh,
		convert_folder_required,
		convert_dimension_required,
		metadata_fields_required,
		no_items_selected_message,
		words.text_false,
		words.text_true,
		platform_text.title_error,
		title_folder,
		title_rate,
		title_updating,
		tooltip_color_reset,
		tooltip_edit1,
		tooltip_fullscreen,
		tooltip_language,
		tooltip_nav_bar,
		tooltip_open,
		tooltip_pin,
		tooltip_play,
		tooltip_slideshow,
		tooltip_rotate_reset,
		tooltip_scale_up,
		tooltip_tag_with,
		tooltip_toggle_details_selected,
		tooltip_toggle_details_all,
		tooltip_tools,
		tooltip_view_menu,
		total_title,
		truncated_at_one_mb,
		type_to_search,
		unknown,
		unselect_fmt,
		update,
		update_install_now,
		update_checking,
		update_up_to_date_fmt,
		update_check_failed,
		update_avail_version_fmt,
		update_available,
		update_current_version_fmt,
		update_failed,
		update_help,
		update_help_fmt,
		update_more_info,
		update_more_info_help,
		update_not_now,
		update_not_now_help,
		update_please_wait,
		update_title,
		version,
		vibrance,
		video,
		videos,
		platform_text.webp_best,
		xmp_metadata_title,
		icc_metadata_title,
		zoom_kb,
		scope,
		value,
		command_volume200,
		command_volume100,
		command_volume75,
		command_volume50,
		command_volume25,
		command_volume0,
		command_autoplay,
		command_auto_advance,
		auto_advance_help,
		command_last_played_pos,
		command_repeat_one,
		command_repeat_all,
		command_repeat_none,
		command_playback_menu,
		command_playback_toolbar,
		command_slideshow,
		command_filter_items,
		command_filter_photos,
		command_filter_videos,
		command_filter_audio,
		some_items_filtered_fmt,
		items_created_on_fmt,
		command_all_tags,
		command_favorite_tags,
		option_favorite_tags,
		tags_workflow_label,
		configure_favorite_tags,
		file,
		source,
		destination,
		action,
		old_name,
		new_name,
		local,
		remote,
		status,
		message,
		import,
		exists,
		previously_imported,
		ignore,
		view_empty_message,
	};

	_all_plurals = std::vector<std::reference_wrapper<plural_text>>
	{
		rotate_info_fmt,
		title_folder_count_fmt,
		title_item_count_fmt,
		map_items_here_fmt,
		rating_set_fmt,
		cannot_process_fmt,
		rename_fmt,
		dup_count_fmt,
		sidecar_count_fmt,
		processed_x_of_x_fmt,
		processed_fmt,
		failed_items_fmt,
		ignored_fmt,
		canceled_items_fmt,
		ignored_exist_already_fmt,
		ignored_previous_fmt,
		import_date_unset_fmt,
		delete_info_fmt,
		delete_info_permanent_fmt,
		delete_many_warning_fmt,
		copy_fmt,
		move_fmt,
		be_updated_fmt,
		edit_metadata_fmt,
		burn_info_fmt,
		convert_info_fmt,
		tag_info_fmt,
		adjust_date_info_fmt,
		email_info_fmt,
		share_info_fmt,
		would_overwrite_fmt,
		gps_overwrite_count_fmt,
		movie_ignored_fmt,
		movie_missing_fmt,
		movie_relink_fmt,
	};

	text_mapping mapping;

	for (auto&& e : _all_texts)
	{
		mapping.insert(
			std::make_pair<std::string_view, std::reference_wrapper<text_t>>(
				std::string_view{e.get().text}, e.get()));
	}

	for (auto&& p : _all_plurals)
	{
		mapping.insert(
			std::make_pair<std::string_view, std::reference_wrapper<text_t>>(
				std::string_view{p.get().one.text}, p.get().one));
		mapping.insert(
			std::make_pair<std::string_view, std::reference_wrapper<text_t>>(
				std::string_view{p.get().plural.text}, p.get().plural));
	}

	_text_mapping = std::move(mapping);
}

std::string app_text_t::translate_text(const std::string& text, const std::string_view scope) const
{
	if (!scope.empty())
	{
		const auto found = _text_mapping.find(std::format("{}//{}", text, scope));

		if (found != _text_mapping.end())
		{
			return std::string(tt_prep(found->second.get().sv()));
		}
	}

	const auto found = _text_mapping.find(text);
	return found != _text_mapping.end()
		       ? std::string(tt_prep(found->second.get().sv()))
		       : std::string(tt_prep(text));
}

std::vector<std::string> app_text_t::add_translate_text(const std::vector<str::cached>& text,
                                                        const std::string_view scope) const
{
	df::hash_set<std::string, df::ihash, df::ieq> result;
	result.reserve(text.size() * 2);

	for (const auto& t : text)
	{
		result.emplace(t.sv());
		result.emplace(translate_text(t.str(), scope));
	}

	return {result.begin(), result.end()};
}

// Classification of the built-in genre names. A genre is offered for a selection only when it
// belongs to media of that kind, so a photo selection is not asked to choose between Death Metal
// and Euro-Techno. Names that are ordinary words in every medium are marked for all three.
std::vector<std::string> app_text_t::known_genres(const genre_kind kinds) const
{
	constexpr auto ga = genre_kind::audio;
	constexpr auto gv = genre_kind::video;
	constexpr auto gp = genre_kind::photo;

	const std::pair<std::reference_wrapper<const text_t>, genre_kind> table[] = {
		{genre_a_capella, ga}, {genre_abstract, gp}, {genre_acidjazz, ga}, {genre_acidpunk, ga},
		{genre_acid, ga}, {genre_acoustic, ga}, {genre_action_adventure, gv}, {genre_action, gv},
		{genre_aerial, gp}, {genre_alternative, ga}, {genre_alternrock, ga}, {genre_ambient, ga},
		{genre_analog, gp}, {genre_animation, gv}, {genre_anime, gv}, {genre_architectural, gp},
		{genre_avantgarde, ga}, {genre_aviation, gp}, {genre_ballad, ga}, {genre_bass, ga},
		{genre_bebob, ga}, {genre_bigband, ga}, {genre_bluegrass, ga}, {genre_blues, ga},
		{genre_booty_bass, ga}, {genre_brazilian, ga}, {genre_cabaret, ga}, {genre_candid, gp},
		{genre_celtic, ga}, {genre_chambermusic, ga}, {genre_chanson, ga}, {genre_childrens, ga | gv},
		{genre_chorus, ga}, {genre_christian_gospel, ga}, {genre_christian_rap, ga},
		{genre_classic_rock, ga}, {genre_classic, ga}, {genre_classical, ga}, {genre_close_up, gp},
		{genre_cloudscape, gp}, {genre_club, ga}, {genre_comedy, ga | gv}, {genre_conceptual, gp},
		{genre_concert_films, gv}, {genre_concert, gv | gp}, {genre_conservation, gp},
		{genre_country, ga}, {genre_cult, gv}, {genre_dance_hall, ga}, {genre_dance, ga},
		{genre_darkwave, ga}, {genre_death_metal, ga}, {genre_disco, ga}, {genre_documentary, gv},
		{genre_drama, gv}, {genre_dream, ga}, {genre_drum_solo, ga}, {genre_duet, ga},
		{genre_easylistening, ga}, {genre_electronic, ga}, {genre_ethnic, ga}, {genre_euro_dance, ga},
		{genre_euro_house, ga}, {genre_euro_techno, ga}, {genre_family, gv}, {genre_fashion, gp},
		{genre_fast_fusion, ga}, {genre_film_still, gp}, {genre_fine_art, gp}, {genre_fire, gp},
		{genre_fireworks, gp}, {genre_fitness_workout, gv}, {genre_folk, ga}, {genre_folklore, ga},
		{genre_folk_rock, ga}, {genre_food, gp}, {genre_foreign, gv}, {genre_forensic, gp},
		{genre_freestyle, ga}, {genre_funk, ga}, {genre_fusion, ga}, {genre_game, ga | gv},
		{genre_gangsta, ga}, {genre_geophotography, gp}, {genre_glamour, gp}, {genre_gospel, ga},
		{genre_gothic_rock, ga}, {genre_gothic, ga}, {genre_grunge, ga}, {genre_hardrock, ga},
		{genre_high_key, gp}, {genre_high_speed, gp}, {genre_hip_hop, ga}, {genre_hip_hop_rap, ga},
		{genre_holiday, genre_kind::any}, {genre_horror, gv}, {genre_house, ga},
		{genre_humour, genre_kind::any}, {genre_independent, gv}, {genre_industrial, ga},
		{genre_instrumental_pop, ga}, {genre_instrumental_rock, ga}, {genre_instrumental, ga},
		{genre_jazz, ga}, {genre_jazzfun, ga}, {genre_jungle, ga}, {genre_kids_family, gv},
		{genre_kids, gv}, {genre_kirlian, gp}, {genre_landscape, gp}, {genre_latin, ga},
		{genre_lifestyle, gp}, {genre_lo_fi, ga}, {genre_lomography, gp}, {genre_long_exposure, gp},
		{genre_low_key, gp}, {genre_macro, gp}, {genre_medical, gp}, {genre_meditative, ga},
		{genre_metal, ga}, {genre_monochrome, gp}, {genre_music_documentaries, gv},
		{genre_music_feature_films, gv}, {genre_musical, ga | gv}, {genre_musicals, gv},
		{genre_narrative, gv | gp}, {genre_national_folk, ga}, {genre_native_american, ga},
		{genre_new_age, ga}, {genre_new_wave, ga}, {genre_night, gp}, {genre_noise, ga},
		{genre_nonfiction, gv}, {genre_oldies, ga}, {genre_opera, ga}, {genre_other, genre_kind::any},
		{genre_panorama, gp}, {genre_panoramic, gp}, {genre_film_noir, gv}, {genre_photo_op, gp},
		{genre_photobiography, gp}, {genre_photojournalism, gp}, {genre_photowalking, gp},
		{genre_podcast, ga | gv}, {genre_polaroid, gp}, {genre_polka, ga}, {genre_pop, ga},
		{genre_pop_funk, ga}, {genre_pop_folk, ga}, {genre_porn_groove, ga}, {genre_portrait, gp},
		{genre_power_ballad, ga}, {genre_pranks, gv}, {genre_primus, ga}, {genre_progressive_rock, ga},
		{genre_psychadelic, ga}, {genre_psychedelic_rock, ga}, {genre_punk_rock, ga}, {genre_punk, ga},
		{genre_randb, ga}, {genre_rap, ga}, {genre_rave, ga}, {genre_reality_tv, gv},
		{genre_reggae, ga}, {genre_retro, ga | gp}, {genre_revival, ga}, {genre_rhythmic_soul, ga},
		{genre_rock_and_roll, ga}, {genre_rock, ga}, {genre_romance, gv}, {genre_samba, ga},
		{genre_satellite, gp}, {genre_satire, gv}, {genre_scifi_and_fantasy, gv},
		{genre_short_films, gv}, {genre_show_tunes, ga}, {genre_singer_songwriter, ga}, {genre_ska, ga},
		{genre_slow_jam, ga}, {genre_slow_rock, ga}, {genre_social, gp}, {genre_soft_focus, gp},
		{genre_sonata, ga}, {genre_soul, ga}, {genre_sound_clip, ga}, {genre_soundtrack, ga | gv},
		{genre_southern_rock, ga}, {genre_space, gp}, {genre_special_interest, gv}, {genre_speech, ga},
		{genre_sports, gv | gp}, {genre_star_trail, gp}, {genre_still_life, gp}, {genre_stock, gp},
		{genre_street, gp}, {genre_subminiature, gp}, {genre_swing, ga}, {genre_symphonic_rock, ga},
		{genre_symphony, ga}, {genre_tango, ga}, {genre_techno, ga}, {genre_techno_industrial, ga},
		{genre_teens, gv}, {genre_thriller, gv}, {genre_time_lapse, gv | gp}, {genre_top40, ga},
		{genre_trailer, gv}, {genre_trance, ga}, {genre_travel, gv | gp}, {genre_tribal, ga},
		{genre_trip_hop, ga}, {genre_ultraviolet, gp}, {genre_underwater, gp},
		{genre_unknown, genre_kind::any}, {genre_urban, ga | gp}, {genre_vernacular, gp},
		{genre_vintage, gp}, {genre_vocal, ga}, {genre_war, gv}, {genre_western, gv}, {genre_world, ga},
	};

	std::vector<std::string> result;
	result.reserve(std::size(table));

	for (const auto& entry : table)
	{
		if (entry.second && kinds)
		{
			result.emplace_back(entry.first.get().sv());
		}
	}

	std::ranges::sort(result, str::iless());
	return result;
}


app_text_t tt;

// The foundation and the platform layer write their words and messages with tt's translations of them.
[[maybe_unused]] static const auto s_text_installed =
	(str::install_words(&tt.words), platform::install_text(&tt.platform_text), true);
