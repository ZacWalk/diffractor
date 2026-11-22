// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Tutorial scripts (app_tutorial.h has their grammar): the reader of a .dft, how long a beat's narration
// lasts - measured by tutorial.py, or guessed - and the player that acts a script out in the window, its pointer and
// keys given to the window as its own input and its pointer drawn by the recorder, while the window is filmed.

#include "pch.h"

#include "model_index.h"
#include "model.h"
#include "ui_dialog.h"
#include "view_items.h"
#include "app_sidebar.h"
#include "app_command_line.h"
#include "app.h"

#include <charconv>
#include <cstdio>

namespace
{
	// The pace a narration is guessed at before it has a voice: about 150 words a minute, and a breath after.
	constexpr double guessed_characters_per_second = 15.0;
	constexpr double guessed_breath_seconds = 0.4;
	// The pause between one beat's narration and the next's.
	constexpr double beat_gap_seconds = 0.6;
	// The window as it is, before the first beat and after the last.
	constexpr double lead_in_seconds = 1.5;
	constexpr double lead_out_seconds = 2.0;
	constexpr double glide_seconds = 0.8;
	constexpr double click_seconds = 0.5;
	constexpr double double_click_seconds = 0.7;
	constexpr double key_seconds = 0.4;
	constexpr double typing_characters_per_second = 11.0;
	constexpr double typed_seconds = 0.3;
	constexpr double open_seconds = 0.3;
	// What a notch of the wheel turns it by, in the platform's units.
	constexpr int wheel_detent = 120;
	// Settled is quiet for this long; a settle gives up after its limit, which for the scene is long enough for the
	// collection's first index.
	constexpr double settle_quiet_seconds = 0.5;
	constexpr double settle_limit_seconds = 30.0;
	constexpr double scene_settle_limit_seconds = 600.0;

	struct script_field
	{
		std::string text;
		bool quoted = false;
	};

	bool is_space(const char c)
	{
		return c == ' ' || c == '\t';
	}

	bool is_name(const std::string_view text)
	{
		return !text.empty() && std::ranges::all_of(text, [](const char c)
		{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; });
	}

	// The fields of a line, quoted as .dfv quotes them: "double" taken as it stands, 'single' reading \n, \' and \\.
	// Nothing for a quote left open, or one run into the next field.
	std::optional<std::vector<script_field>> split_line(const std::string_view line)
	{
		std::vector<script_field> result;
		size_t i = 0;

		while (true)
		{
			while (i < line.size() && is_space(line[i])) ++i;
			if (i >= line.size()) break;

			script_field field;

			if (line[i] == '"')
			{
				const auto close = line.find('"', i + 1);
				if (close == std::string_view::npos) return std::nullopt;
				field.text = line.substr(i + 1, close - i - 1);
				field.quoted = true;
				i = close + 1;
			}
			else if (line[i] == '\'')
			{
				field.quoted = true;
				auto closed = false;
				++i;

				while (i < line.size())
				{
					const auto c = line[i++];

					if (c == '\'')
					{
						closed = true;
						break;
					}

					if (c == '\\' && i < line.size() &&
					    (line[i] == 'n' || line[i] == '\'' || line[i] == '\\' || line[i] == '"'))
					{
						field.text += line[i] == 'n' ? '\n' : line[i];
						++i;
						continue;
					}

					field.text += c;
				}

				if (!closed) return std::nullopt;
			}
			else
			{
				const auto begin = i;
				while (i < line.size() && !is_space(line[i])) ++i;
				field.text = line.substr(begin, i - begin);
			}

			if (field.quoted && i < line.size() && !is_space(line[i])) return std::nullopt;
			result.emplace_back(std::move(field));
		}

		return result;
	}

	std::optional<double> parse_number(const std::string_view text)
	{
		const auto dots = std::ranges::count(text, '.');
		const auto digits = std::ranges::count_if(text, [](const char c) { return c >= '0' && c <= '9'; });
		if (digits == 0 || dots > 1 || digits + dots != static_cast<std::ptrdiff_t>(text.size())) return {};

		auto value = 0.0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return {};
		return value;
	}

	size_t count_characters(const std::string_view text)
	{
		return static_cast<size_t>(
			std::ranges::count_if(text, [](const char c) { return (static_cast<uint8_t>(c) & 0xC0) != 0x80; }));
	}

	struct cue_span
	{
		size_t begin = 0;
		size_t end = 0;
		std::string name;
	};

	// Each {name} in a narration.
	std::vector<cue_span> find_cues(const std::string_view narration)
	{
		std::vector<cue_span> result;
		size_t i = 0;

		while ((i = narration.find('{', i)) != std::string_view::npos)
		{
			const auto close = narration.find('}', i + 1);
			if (close == std::string_view::npos) break;

			const auto name = narration.substr(i + 1, close - i - 1);

			if (is_name(name))
			{
				result.push_back({i, close + 1, std::string(name)});
				i = close + 1;
			}
			else
			{
				++i;
			}
		}

		return result;
	}

	struct named_command
	{
		std::string_view name;
		commands command;
	};

	constexpr named_command named_commands[] = {
		{"back", commands::browse_back},
		{"forward", commands::browse_forward},
		{"next", commands::browse_next_item},
		{"previous", commands::browse_previous_item},
		{"next-group", commands::browse_next_group},
		{"previous-group", commands::browse_previous_group},
		{"parent", commands::browse_parent},
		{"search", commands::browse_search},
		{"items", commands::view_items},
		{"fullscreen", commands::view_fullscreen},
		{"sidebar", commands::view_show_sidebar},
		{"details", commands::option_toggle_details},
		{"zoom", commands::view_zoom},
		{"zoom-in", commands::view_zoom_in},
		{"zoom-out", commands::view_zoom_out},
		{"play", commands::play},
		{"slideshow", commands::slideshow},
		{"rate-0", commands::rate_none},
		{"rate-1", commands::rate_1},
		{"rate-2", commands::rate_2},
		{"rate-3", commands::rate_3},
		{"rate-4", commands::rate_4},
		{"rate-5", commands::rate_5},
		{"reject", commands::rate_rejected},
		{"label-select", commands::label_select},
		{"label-second", commands::label_second},
		{"label-approved", commands::label_approved},
		{"label-review", commands::label_review},
		{"label-to-do", commands::label_to_do},
		{"label-none", commands::label_none},
		{"pin", commands::pin_item},
		{"related", commands::search_related},
		{"select-all", commands::select_all},
		{"select-none", commands::select_nothing},
		{"group-taken", commands::group_original},
		{"group-type", commands::group_file_type},
		{"group-folder", commands::group_folder},
		{"group-location", commands::group_location},
		{"group-rating", commands::group_rating},
		{"group-camera", commands::group_camera},
		{"sort-name", commands::sort_name},
		{"sort-taken", commands::sort_date_original},
		{"sort-size", commands::sort_size},
		{"filter-photos", commands::filter_photos},
		{"filter-videos", commands::filter_videos},
		{"filter-audio", commands::filter_audio},
		{"tag", commands::tool_tag},
		{"edit", commands::tool_edit},
		{"metadata", commands::tool_edit_metadata},
		{"description", commands::tool_edit_description},
		{"adjust-date", commands::tool_adjust_date},
		{"rename", commands::tool_rename},
		{"convert", commands::tool_convert},
		{"locate", commands::tool_locate},
		{"explore", commands::explore},
		{"rotate-clockwise", commands::tool_rotate_clockwise},
		{"rotate-anticlockwise", commands::tool_rotate_anticlockwise},
		{"auto-color", commands::edit_item_auto_color},
		{"auto-straighten", commands::edit_item_auto_straighten},
		{"auto-document", commands::edit_item_auto_document},
		{"save", commands::edit_item_save},
		{"movie", commands::tool_movie},
		{"movie-add-text", commands::tool_movie_add_text},
		{"import", commands::tool_import},
		{"sync", commands::tool_sync},
		{"share", commands::tool_share},
		{"run", commands::tool_run},
		{"close", commands::view_close},
		{"cancel", commands::view_cancel},
		{"options", commands::options},
	};

	std::optional<char32_t> key_code(const std::string_view name)
	{
		if (name.size() == 1)
		{
			const auto c = name[0];
			if (c >= 'a' && c <= 'z') return static_cast<char32_t>(c - 'a' + 'A');
			if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<char32_t>(c);
			return {};
		}

		// The platform's codes are its own, so they are looked up as the script is read rather than tabled.
		const std::pair<std::string_view, const char32_t*> named[] = {
			{"enter", &platform::keys::RETURN},   {"escape", &platform::keys::ESCAPE},
			{"space", &platform::keys::SPACE},    {"tab", &platform::keys::TAB},
			{"backspace", &platform::keys::BACK}, {"delete", &platform::keys::DEL},
			{"left", &platform::keys::LEFT},      {"right", &platform::keys::RIGHT},
			{"up", &platform::keys::UP},          {"down", &platform::keys::DOWN},
			{"home", &platform::keys::HOME},      {"end", &platform::keys::END},
			{"page-up", &platform::keys::PRIOR},  {"page-down", &platform::keys::NEXT},
			{"insert", &platform::keys::INSERT},  {"apps", &platform::keys::APPS},
			{"plus", &platform::keys::OEM_PLUS},  {"minus", &platform::keys::OEM_MINUS},
			{"f1", &platform::keys::F1},          {"f2", &platform::keys::F2},
			{"f3", &platform::keys::F3},          {"f4", &platform::keys::F4},
			{"f5", &platform::keys::F5},          {"f6", &platform::keys::F6},
			{"f7", &platform::keys::F7},          {"f8", &platform::keys::F8},
			{"f9", &platform::keys::F9},          {"f10", &platform::keys::F10},
			{"f11", &platform::keys::F11},        {"f12", &platform::keys::F12},
		};

		for (const auto& [key_name, code] : named)
		{
			if (core::icmp(key_name, name) == 0) return *code;
		}

		return {};
	}

	// ctrl+, shift+ and alt+, in any order, then the key.
	bool parse_key(std::string_view chord, char32_t& key, platform::key_state& keys)
	{
		for (auto plus = chord.find('+'); plus != std::string_view::npos && plus + 1 < chord.size();
		     plus = chord.find('+'))
		{
			const auto modifier = chord.substr(0, plus);
			if (core::icmp(modifier, "ctrl") == 0) keys.control = true;
			else if (core::icmp(modifier, "shift") == 0) keys.shift = true;
			else if (core::icmp(modifier, "alt") == 0) keys.alt = true;
			else return false;
			chord.remove_prefix(plus + 1);
		}

		const auto code = key_code(chord);
		if (code) key = *code;
		return code.has_value();
	}

	std::string read_text(const core::file_path path)
	{
		const auto file = platform::open_file(path, platform::file_open_mode::read);
		if (!file) return {};

		constexpr uint64_t max_script_bytes = 4 * 1024 * 1024;
		const auto size = file->size();
		if (size == 0 || size > max_script_bytes) return {};

		std::string result(static_cast<size_t>(size), '\0');
		const auto read = file->read(std::bit_cast<uint8_t*>(result.data()), size);
		result.resize(static_cast<size_t>(read));
		return result;
	}

	// One line at a time, without its line break.
	template <typename F> void for_each_line(std::string_view text, F&& f)
	{
		if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
		auto number = 0;

		while (!text.empty())
		{
			const auto end = text.find('\n');
			auto line = text.substr(0, end);
			text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
			if (line.ends_with('\r')) line.remove_suffix(1);
			f(line, ++number);
		}
	}
}

std::optional<commands> tutorial_command(const std::string_view name)
{
	for (const auto& [short_name, command] : named_commands)
	{
		if (core::icmp(short_name, name) == 0) return command;
	}

	// Any command by its full name, as the agent tools name it.
	return command_from_name(name);
}

bool parse_tutorial_key(const std::string_view chord, char32_t& key, platform::key_state& keys)
{
	return parse_key(chord, key, keys);
}

std::optional<double> tutorial_narration::cue(const std::string_view name) const
{
	for (const auto& [cue_name, cue_seconds] : cues)
	{
		if (cue_name == name) return cue_seconds;
	}

	return {};
}

std::string tutorial_spoken_text(const std::string_view narration)
{
	std::string spoken;
	size_t at = 0;

	for (const auto& cue : find_cues(narration))
	{
		spoken += narration.substr(at, cue.begin - at);
		at = cue.end;
	}

	spoken += narration.substr(at);

	// A cue leaves the spaces either side of it, and one before a stop: they close up to what was meant.
	std::string result;

	for (const auto c : spoken)
	{
		if (is_space(c) && (result.empty() || is_space(result.back()))) continue;
		if ((c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') && !result.empty() &&
		    is_space(result.back()))
			result.pop_back();
		result += is_space(c) ? ' ' : c;
	}

	while (!result.empty() && is_space(result.back())) result.pop_back();
	return result;
}

tutorial_narration estimate_tutorial_narration(const std::string_view narration)
{
	tutorial_narration result;
	const auto spoken = static_cast<double>(count_characters(tutorial_spoken_text(narration)));
	const auto speaking = spoken / guessed_characters_per_second;
	result.seconds = speaking + guessed_breath_seconds;

	for (const auto& cue : find_cues(narration))
	{
		const auto before = static_cast<double>(count_characters(tutorial_spoken_text(narration.substr(0, cue.begin))));
		result.cues.emplace_back(cue.name, spoken > 0.0 ? speaking * before / spoken : 0.0);
	}

	return result;
}

std::optional<tutorial_narration> parse_tutorial_timing(const std::string_view text)
{
	tutorial_narration result;
	result.measured = true;
	auto has_length = false;

	for_each_line(text, [&](const std::string_view line, int)
	{
		std::vector<std::string_view> parts;

		for (size_t at = 0; at <= line.size();)
		{
			const auto tab = line.find('\t', at);
			parts.emplace_back(line.substr(at, tab == std::string_view::npos ? std::string_view::npos : tab - at));
			if (tab == std::string_view::npos) break;
			at = tab + 1;
		}

		if (parts.size() >= 2 && parts[0] == "length")
		{
			if (const auto seconds = parse_number(parts[1]))
			{
				result.seconds = *seconds;
				has_length = true;
			}
		}
		else if (parts.size() >= 3 && parts[0] == "cue" && is_name(parts[1]))
		{
			if (const auto seconds = parse_number(parts[2])) result.cues.emplace_back(std::string(parts[1]), *seconds);
		}
	});

	if (!has_length) return {};
	return result;
}

tutorial_script parse_tutorial_script(const std::string_view text)
{
	tutorial_script script;

	for_each_line(text, [&script](std::string_view line, const int number)
	{
		while (!line.empty() && is_space(line.front())) line.remove_prefix(1);
		if (line.empty() || line.front() == '#') return;

		const auto problem = [&](const std::string_view what)
		{
			script.problems.emplace_back(std::format("line {}: {}", number, what));
		};

		const auto split = split_line(line);

		if (!split || split->empty() || split->front().quoted)
		{
			problem("a quote is left open, or runs into what follows it");
			return;
		}

		const auto& fields = *split;
		const auto& verb = fields[0].text;
		const auto word = [&fields](const size_t i) -> std::optional<std::string_view>
		{
			if (i >= fields.size() || fields[i].quoted) return {};
			return fields[i].text;
		};
		const auto quoted = [&fields](const size_t i) -> std::optional<std::string_view>
		{
			if (i >= fields.size() || !fields[i].quoted) return {};
			return fields[i].text;
		};
		// The field at i as a number, where it is one.
		const auto numeric = [&word](const size_t i) -> std::optional<double>
		{
			const auto field = word(i);
			return field ? parse_number(*field) : std::nullopt;
		};

		if (verb == "say")
		{
			const auto id = word(1);
			const auto narration = quoted(2);

			if (!id || !is_name(*id) || !narration || fields.size() != 3)
			{
				problem("say takes an id - letters, digits, - and _ - and its narration in quotes");
				return;
			}

			if (std::ranges::any_of(script.beats, [&](const tutorial_beat& b) { return b.id == *id; }))
			{
				problem(std::format("another beat is already {}", *id));
				return;
			}

			tutorial_beat beat;
			beat.id = *id;
			beat.narration = *narration;
			beat.line = number;
			script.beats.emplace_back(std::move(beat));
			return;
		}

		tutorial_step step;
		step.line = number;
		auto used = size_t{1};

		if (verb == "cue")
		{
			step.kind = tutorial_step_kind::cue;
			const auto name = word(1);

			if (script.beats.empty() || !name)
			{
				problem("cue names a {cue} in its beat's narration");
				return;
			}

			const auto cues = find_cues(script.beats.back().narration);

			if (std::ranges::none_of(cues, [&](const cue_span& c) { return c.name == *name; }))
			{
				problem(std::format("the narration has no {{{}}}", *name));
				return;
			}

			step.text = *name;
			used = 2;
		}
		else if (verb == "wait")
		{
			step.kind = tutorial_step_kind::wait;
			const auto seconds = numeric(1);

			if (!seconds)
			{
				problem("wait takes a number of seconds");
				return;
			}

			step.seconds = *seconds;
			used = 2;
		}
		else if (verb == "settle")
		{
			step.kind = tutorial_step_kind::settle;
		}
		else if (verb == "point" || verb == "drag")
		{
			step.kind = verb == "point" ? tutorial_step_kind::point : tutorial_step_kind::drag;
			const auto name = word(1);
			auto& anchor = step.anchor;
			used = 2;

			static constexpr std::pair<std::string_view, tutorial_anchor_kind> places[] = {
				{"address", tutorial_anchor_kind::address},   {"sidebar", tutorial_anchor_kind::sidebar},
				{"types", tutorial_anchor_kind::types},       {"map", tutorial_anchor_kind::map},
				{"calendar", tutorial_anchor_kind::calendar}, {"items", tutorial_anchor_kind::items},
				{"preview", tutorial_anchor_kind::preview},   {"view", tutorial_anchor_kind::view},
			};

			const auto place = name ? std::ranges::find_if(places, [&](const auto& p) { return p.first == *name; })
			                        : std::ranges::end(places);

			if (place != std::ranges::end(places))
			{
				anchor.kind = place->second;
			}
			else if (name == "month")
			{
				anchor.kind = tutorial_anchor_kind::month;
				const auto month = word(2).value_or("");
				const auto year = month.size() == 7 && month[4] == '-' ? parse_number(month.substr(0, 4))
				                                                       : std::nullopt;
				const auto number_of_month = month.size() == 7 ? parse_number(month.substr(5, 2)) : std::nullopt;

				if (!year || !number_of_month || *number_of_month < 1 || *number_of_month > 12)
				{
					problem(std::format("{} month takes the month as yyyy-mm", verb));
					return;
				}

				anchor.year = static_cast<int>(*year);
				anchor.month = static_cast<int>(*number_of_month);
				used = 3;
			}
			else if (name == "button")
			{
				anchor.kind = tutorial_anchor_kind::button;
				const auto command = word(2) ? tutorial_command(*word(2)) : std::nullopt;

				if (!command)
				{
					problem(std::format("{} button takes the name of a command", verb));
					return;
				}

				anchor.command = *command;
				used = 3;
			}
			else if (name == "tile")
			{
				anchor.kind = tutorial_anchor_kind::tile;
				const auto file = quoted(2);

				if (!file || file->empty())
				{
					problem(std::format("{} tile takes a file name in quotes", verb));
					return;
				}

				anchor.name = *file;
				anchor.x = 0.5;
				anchor.y = 0.5;
				used = 3;

				// Two numbers more are where in the tile, as fractions of it; one alone is the glide.
				const auto x = numeric(3);
				const auto y = numeric(4);

				if (x && y)
				{
					if (*x > 1.0 || *y > 1.0)
					{
						problem("a point in a tile is x and y, each from 0 to 1");
						return;
					}

					anchor.x = *x;
					anchor.y = *y;
					used = 5;
				}
			}
			else if (name == "at")
			{
				anchor.kind = tutorial_anchor_kind::at;
				const auto x = numeric(2);
				const auto y = numeric(3);

				if (!x || !y || *x > 1.0 || *y > 1.0)
				{
					problem(std::format("{} at takes x and y, each from 0 to 1", verb));
					return;
				}

				anchor.x = *x;
				anchor.y = *y;
				used = 4;
			}
			else
			{
				problem(std::format("{} to what? {} is no anchor", verb, name.value_or("")));
				return;
			}

			if (const auto seconds = word(used))
			{
				const auto glide = parse_number(*seconds);

				if (!glide)
				{
					problem("a pointer glides for a number of seconds");
					return;
				}

				step.seconds = *glide;
				++used;
			}
		}
		else if (verb == "hide")
		{
			step.kind = tutorial_step_kind::hide;
		}
		else if (verb == "click" || verb == "double-click" || verb == "right-click")
		{
			step.kind = verb == "click"          ? tutorial_step_kind::click
			            : verb == "double-click" ? tutorial_step_kind::double_click
			                                     : tutorial_step_kind::right_click;

			for (; word(used); ++used)
			{
				if (*word(used) == "ctrl") step.keys.control = true;
				else if (*word(used) == "shift") step.keys.shift = true;
				else break;
			}
		}
		else if (verb == "wheel")
		{
			step.kind = tutorial_step_kind::wheel;
			const auto way = word(1);
			const auto notches = word(2) ? numeric(2) : std::optional<double>(1.0);

			if ((way != "up" && way != "down") || !notches || *notches < 1.0 || *notches > 50.0)
			{
				problem("wheel turns up or down, by a number of notches from 1 to 50");
				return;
			}

			step.notches = static_cast<int>(*notches) * (way == "up" ? 1 : -1);
			used = word(2) ? 3 : 2;
		}
		else if (verb == "type" || verb == "text")
		{
			step.kind = verb == "type" ? tutorial_step_kind::type : tutorial_step_kind::text;
			const auto typed = quoted(1);

			if (!typed)
			{
				problem(std::format("{} takes its text in quotes", verb));
				return;
			}

			step.text = *typed;
			used = 2;
		}
		else if (verb == "key")
		{
			step.kind = tutorial_step_kind::key;

			if (!word(1) || !parse_key(*word(1), step.key, step.keys))
			{
				problem(std::format("key {} is no key", word(1).value_or("")));
				return;
			}

			used = 2;
		}
		else if (verb == "command")
		{
			step.kind = tutorial_step_kind::command;
			const auto command = word(1) ? tutorial_command(*word(1)) : std::nullopt;

			if (!command)
			{
				problem(std::format("command {} is not one a script can run", word(1).value_or("")));
				return;
			}

			step.command = *command;
			used = 2;
		}
		else if (verb == "open")
		{
			step.kind = tutorial_step_kind::open;

			if (const auto opened = quoted(1))
			{
				step.text = *opened;
				used = 2;
			}
		}
		else if (verb == "select")
		{
			step.kind = tutorial_step_kind::select;
			const auto file = quoted(1);

			if (!file || file->empty())
			{
				problem("select takes a file name in quotes");
				return;
			}

			step.text = *file;
			used = 2;

			if (word(2) == "ctrl")
			{
				step.keys.control = true;
				used = 3;
			}
		}
		else if (verb == "tooltips")
		{
			step.kind = tutorial_step_kind::tooltips;

			if (word(1) != "on" && word(1) != "off")
			{
				problem("tooltips is on or off");
				return;
			}

			step.on = word(1) == "on";
			used = 2;
		}
		else if (verb == "list")
		{
			step.kind = tutorial_step_kind::list;
		}
		else
		{
			problem(std::format("{} is not a step", verb));
			return;
		}

		if (used < fields.size())
		{
			problem(std::format("{} has more than it takes", verb));
			return;
		}

		(script.beats.empty() ? script.scene : script.beats.back().steps).emplace_back(std::move(step));
	});

	return script;
}

tutorial_player::tutorial_player(app_frame& app, tutorial_options options) : _app(app), _options(std::move(options))
{
	_started = now();
	load();
}

tutorial_player::~tutorial_player() = default;

double tutorial_player::now() const
{
	return core::now();
}

void tutorial_player::say(const std::string_view text) const
{
	core::log(__FUNCTION__, text);
	if (_options.said) _options.said(text);
}

std::string tutorial_player::shown() const
{
	const auto& state = _app._state;
	const auto view = state.view_mode() == view_type::items   ? "items"
	                  : state.view_mode() == view_type::media ? (state.is_full_screen ? "full screen" : "media")
	                                                          : "a task";
	return std::format("{}, '{}', {} selected", view, state.search().text(), state.selected_items().size());
}

void tutorial_player::load()
{
	if (!_options.steps_only)
	{
		say(_options.script.is_empty() ? "Tutorial"s : std::format("Tutorial {}", _options.script));
	}

	const auto alive = std::weak_ptr<bool>(_alive);

	// The player is only reached back on the window's thread, and only while it lives.
	_app.queue_async(async_queue::work,
	                 [&app = _app, player = this, alive, path = _options.script, given = _options.text]
	{
		loaded result;
		const auto text = given.empty() ? read_text(path) : given;

		if (text.empty())
		{
			result.failure = given.empty() && !path.is_empty()
			                     ? std::format("Tutorial not played: {} could not be read", path)
			                     : "Nothing to play: the script is empty"s;
		}
		else
		{
			result.script = parse_tutorial_script(text);
			const auto voices = path.is_empty()
			                        ? core::folder_path{}
			                        : path.folder().combine("narration").combine(path.file_name_without_extension());

			for (const auto& beat : result.script.beats)
			{
				const auto timing_text = voices.is_empty() ? std::string{}
				                                           : read_text(voices.combine_file(beat.id + ".timing"));
				const auto timing = timing_text.empty() ? std::nullopt : parse_tutorial_timing(timing_text);
				result.narration.emplace_back(timing ? *timing : estimate_tutorial_narration(beat.narration));
			}
		}

		app.queue_ui([player, alive, result = std::move(result)]
		{
			if (alive.lock()) player->loaded_script(result);
		});
	});
}

std::vector<std::string> tutorial_player::refused_steps(const tutorial_script& script) const
{
	std::vector<std::string> refused;
	if (!_options.refusal) return refused;

	// What runs a command: a command step, and a click on the toolbar button a point step has gone to.
	const auto check = [this, &refused](const std::vector<tutorial_step>& steps)
	{
		auto pointed_at = commands::none;

		for (const auto& step : steps)
		{
			auto runs = commands::none;

			if (step.kind == tutorial_step_kind::command) runs = step.command;
			else if (step.kind == tutorial_step_kind::point)
				pointed_at = step.anchor.kind == tutorial_anchor_kind::button ? step.anchor.command : commands::none;
			else if (step.kind == tutorial_step_kind::click || step.kind == tutorial_step_kind::double_click)
				runs = pointed_at;

			if (runs == commands::none) continue;
			if (auto why = _options.refusal(runs); !why.empty())
				refused.push_back(std::format("line {}: {}", step.line, why));
		}
	};

	check(script.scene);
	for (const auto& beat : script.beats) check(beat.steps);
	return refused;
}

void tutorial_player::loaded_script(loaded result)
{
	if (!result.failure.empty())
	{
		finish(result.failure, false);
		return;
	}

	if (!result.script.problems.empty())
	{
		for (const auto& problem : result.script.problems) say(problem);
		finish(std::format("Not played: {} lines could not be read", result.script.problems.size()), false);
		return;
	}

	if (const auto refused = refused_steps(result.script); !refused.empty())
	{
		for (const auto& line : refused) say(line);
		finish(std::format("Not played: {} steps run commands this session may not", refused.size()), false);
		return;
	}

	// An agent's steps are a scene alone: they run at once, with nothing settled but what they ask for.
	if (_options.steps_only)
	{
		if (!result.script.beats.empty())
		{
			finish("Steps are not narrated: say starts a beat, which only tutorial_run plays", false);
			return;
		}

		_script = std::move(result.script);
		enter(stage::setting_scene);
		return;
	}

	if (result.script.beats.empty())
	{
		finish("Tutorial not played: it says nothing - a beat starts with say", false);
		return;
	}

	_script = std::move(result.script);
	_narration = std::move(result.narration);

	// The window is settled before the scene is set and again after, before the recording starts.
	tutorial_step settle;
	settle.kind = tutorial_step_kind::settle;
	_script.scene.insert(_script.scene.begin(), settle);
	_script.scene.emplace_back(settle);

	auto spoken = 0.0;
	auto measured = 0;

	for (const auto& narration : _narration)
	{
		spoken += narration.seconds;
		if (narration.measured) ++measured;
	}

	say(std::format("Tutorial: {} beats, {:.1f} s of narration, {} of them voiced", _script.beats.size(), spoken,
	                measured));
	enter(stage::waiting_for_window);
}

void tutorial_player::enter(const stage s)
{
	_stage = s;
	_stage_started = now();
	_step = 0;
	_step_begun = false;
}

void tutorial_player::tick()
{
	if (_stage == stage::loading || _stage == stage::finished) return;

	// A step that opens a dialog or a menu runs its modal loop inside this call, and that loop ticks the frame - and
	// so this - again. Nothing runs inside itself, and nothing runs under a dialog or menu, whoever opened it: a
	// script cannot close one, and what it would act on is behind it. A command raises command_active while its
	// dialog shows, a menu while it is open, and any dialog disables the window. The script waits until it has gone.
	const auto dialog_is_open = ui::command_active != 0 || (_app._app_frame && !_app._app_frame->is_enabled());

	if (dialog_is_open && _options.steps_only && !_ticking)
	{
		finish("A dialog or menu opened, which the steps cannot see past: answer it with dialog_respond or "
		       "menu_respond",
		       true);
		return;
	}

	if (_ticking || dialog_is_open)
	{
		if (!_waiting)
		{
			_waiting = true;
			const auto* const step = current_step();
			const auto where = step ? std::format("line {}", step->line) : std::string("the tutorial");
			say(std::format("{}: a dialog or menu is open, which a script cannot close; it waits until it is closed",
			                where));
		}

		return;
	}

	_waiting = false;
	_ticking = true;
	const auto ticked = core::scope_exit([this] { _ticking = false; });

	if (_owns_recording && _app._recording && _app._recording->is_finished())
	{
		finish("Tutorial stopped: the recording ended", false);
		return;
	}

	switch (_stage)
	{
	case stage::waiting_for_window:
		// The window is settled at the size it is recorded at before the scene is set.
		if (_app._app_frame && _app._app_frame->is_visible() && now() - _stage_started > 0.5)
		{
			_app.focus_view();
			enter(stage::setting_scene);
		}
		break;

	case stage::setting_scene:
		if (run_steps(_script.scene, now() - _stage_started, nullptr))
		{
			if (_options.steps_only)
			{
				finish(_outcome.missed == 0 ? std::format("Done: {} steps", _script.scene.size())
				                            : std::format("Done, but {} of {} steps found nothing to act on",
				                                          _outcome.missed, _script.scene.size()),
				       _outcome.missed == 0);
				break;
			}

			enter(stage::starting_recording);
		}
		break;

	case stage::starting_recording:
		if (_options.record.is_empty())
		{
			say("Rehearsing: nothing is recorded");
			_film_started = now();
			enter(stage::lead_in);
			break;
		}

		if (!_app._recording)
		{
			screen_recording_request request;
			request.window = _app._app_frame->handle();
			request.path = _options.record;
			request.draw_pointer = false;
			request.present_window = true;

			if (const auto why = _app.start_recording(request); !why.empty())
			{
				finish(std::format("Tutorial not recorded: {}", why), false);
				break;
			}

			_owns_recording = true;
			_outcome.movie = _options.record;
			if (_pointer_shown) _app._recording->pointer(_pointer_to, 0.0);
		}

		// From the first frame: before it, the movie has no time to name.
		if (_app._recording->elapsed() >= 0.0)
		{
			_film_started = now() - _app._recording->elapsed();
			enter(stage::lead_in);
		}
		break;

	case stage::lead_in:
		if (now() - _stage_started >= lead_in_seconds)
		{
			enter(stage::beats);
			_beat = 0;
			_beat_started = now();
			_outcome.beats.emplace_back(_script.beats.front().id, now() - _film_started);
			if (_app._recording) _app._recording->mark(_script.beats.front().id);
			say(std::format("{:8.2f}  {:<14} {}", now() - _film_started, _script.beats.front().id, shown()));
		}
		break;

	case stage::beats:
		if (_beat < _script.beats.size())
		{
			const auto beat_time = now() - _beat_started;
			const auto& narration = _narration[_beat];

			if (run_steps(_script.beats[_beat].steps, beat_time, &narration) &&
			    beat_time >= narration.seconds + beat_gap_seconds)
			{
				if (++_beat >= _script.beats.size())
				{
					say(std::format("{:8.2f}  {:<14} {}", now() - _film_started, "(end)", shown()));
					enter(stage::lead_out);
					break;
				}

				_step = 0;
				_step_begun = false;
				_beat_started = now();
				_outcome.beats.emplace_back(_script.beats[_beat].id, now() - _film_started);
				if (_app._recording) _app._recording->mark(_script.beats[_beat].id);
				say(std::format("{:8.2f}  {:<14} {}", now() - _film_started, _script.beats[_beat].id, shown()));
			}
		}
		break;

	case stage::lead_out:
		if (now() - _stage_started >= lead_out_seconds)
		{
			_outcome.film_seconds = now() - _film_started;
			finish(std::format("Tutorial finished: {} beats, a film of {:.1f} s{}", _script.beats.size(),
			                   _outcome.film_seconds,
			                   _outcome.missed == 0
			                       ? std::string{}
			                       : std::format("; {} steps found nothing to act on", _outcome.missed)),
			       true);
		}
		break;

	default: break;
	}

	if (_stage != stage::finished) hover();
}

const tutorial_step* tutorial_player::current_step() const
{
	const auto* const steps = _stage == stage::setting_scene                           ? &_script.scene
	                          : _stage == stage::beats && _beat < _script.beats.size() ? &_script.beats[_beat].steps
	                                                                                   : nullptr;
	return steps && _step < steps->size() ? &(*steps)[_step] : nullptr;
}

bool tutorial_player::run_steps(const std::vector<tutorial_step>& steps, const double beat_time,
                                const tutorial_narration* narration)
{
	while (_step < steps.size())
	{
		const auto& step = steps[_step];

		if (!_step_begun)
		{
			_step_begun = true;
			_step_started = now();
			_step_seconds = 0.0;
			begin_step(step);
		}

		if (_stage == stage::finished || !step_is_done(step, beat_time, narration)) return false;

		++_step;
		_step_begun = false;
	}

	return true;
}

void tutorial_player::begin_step(const tutorial_step& step)
{
	auto& app = _app;
	const auto not_shown = [this, &step](const std::string_view what)
	{
		++_outcome.missed;
		say(std::format("line {}: {} is not shown", step.line, what));
	};

	switch (step.kind)
	{
	case tutorial_step_kind::cue: break;

	case tutorial_step_kind::wait: _step_seconds = step.seconds; break;

	case tutorial_step_kind::settle: _settled_since = -1.0; break;

	case tutorial_step_kind::point:
		if (const auto at = anchor_point(step.anchor))
		{
			const auto glide = step.seconds > 0.0 ? step.seconds : glide_seconds;
			_step_seconds = _pointer_shown ? glide : 0.0;
			_pointed = step.anchor;
			move_pointer(*at, glide);
		}
		else
		{
			not_shown(step.anchor.kind == tutorial_anchor_kind::tile ? step.anchor.name
			                                                         : "what the pointer is to go to");
		}
		break;

	case tutorial_step_kind::hide:
		_pointed.reset();
		move_pointer({}, 0.0);
		break;

	case tutorial_step_kind::click:
	case tutorial_step_kind::double_click:
	{
		const auto twice = step.kind == tutorial_step_kind::double_click;
		_step_seconds = twice ? double_click_seconds : click_seconds;
		click(twice, step.keys);
	}
	break;

	case tutorial_step_kind::right_click:
		_step_seconds = click_seconds;
		right_click(step.keys);
		break;

	case tutorial_step_kind::drag:
		if (!_pointed || !_pointer_shown)
		{
			++_outcome.missed;
			say(std::format("line {}: a drag starts where the pointer is, which is on nothing", step.line));
		}
		else if (const auto to = anchor_point(step.anchor))
		{
			// Pressed where the pointer is; the press follows the pointer as it glides, and is let go of once there.
			if (send_pointer_input(ui::pointer_input::down, platform::pointer_button::left, step.keys, _pointer_to))
			{
				const auto glide = step.seconds > 0.0 ? step.seconds : glide_seconds;
				_dragging = true;
				_dragged_to = _pointer_to;
				_step_seconds = glide;
				_pointed = step.anchor;
				move_pointer(*to, glide);
			}
			else
			{
				++_outcome.missed;
				say(std::format("line {}: the window took no press to drag", step.line));
			}
		}
		else
		{
			not_shown(step.anchor.kind == tutorial_anchor_kind::tile ? step.anchor.name : "what the drag is to go to");
		}
		break;

	case tutorial_step_kind::wheel:
		_step_seconds = key_seconds;
		wheel(step.notches);
		break;

	case tutorial_step_kind::text:
		_step_seconds = static_cast<double>(count_characters(step.text)) / typing_characters_per_second + typed_seconds;
		if (!ui::send_text(app._app_frame, step.text))
		{
			++_outcome.missed;
			say(std::format("line {}: nothing took the keyboard's text", step.line));
		}
		break;

	case tutorial_step_kind::type:
		if (!app._search_has_focus) app._search_edit->focus();
		app._search_edit->window_text({});
		app.search_text_changed({});
		_typed = 0;
		_step_seconds = static_cast<double>(count_characters(step.text)) / typing_characters_per_second + typed_seconds;
		break;

	case tutorial_step_kind::key:
		_step_seconds = key_seconds;
		key(step.key, step.keys);
		break;

	case tutorial_step_kind::command:
		_step_seconds = key_seconds;
		app.invoke(step.command);
		break;

	case tutorial_step_kind::open:
		_step_seconds = open_seconds;
		if (step.text.empty())
		{
			app._state.open(app._view_frame, df::search_t().add_selector(command_line.folder_path), {});
		}
		else
		{
			app._state.open(app._view_frame, std::string_view(step.text));
		}
		break;

	case tutorial_step_kind::select:
		_step_seconds = open_seconds;
		if (!app._state.select(app._view_frame, std::string_view(step.text), step.keys.control)) not_shown(step.text);
		break;

	case tutorial_step_kind::tooltips: _tooltips = step.on; break;

	case tutorial_step_kind::list:
	{
		constexpr auto most = 40;
		std::string names;
		auto count = 0;

		if (app._state.view_mode() == view_type::items)
		{
			for (const auto& group : app._state.groups())
			{
				for (const auto& item : group->items())
				{
					if (!app._view_items->is_visible(item)) continue;
					if (count++ < most) names += std::format("{}{}", names.empty() ? "" : ", ", item->name().sv());
				}
			}
		}

		const auto more = count > most ? std::format(" and {} more", count - most) : std::string();
		say(std::format("line {}: {}; in view: {}{}", step.line, shown(), names.empty() ? "nothing" : names, more));
	}
	break;
	}
}

bool tutorial_player::step_is_done(const tutorial_step& step, const double beat_time,
                                   const tutorial_narration* narration)
{
	const auto elapsed = now() - _step_started;

	switch (step.kind)
	{
	case tutorial_step_kind::cue:
	{
		const auto at = narration ? narration->cue(step.text) : std::nullopt;
		return !at || beat_time >= *at;
	}

	case tutorial_step_kind::settle:
	{
		const auto limit = _stage == stage::setting_scene ? scene_settle_limit_seconds : settle_limit_seconds;

		if (!is_settled())
		{
			_settled_since = -1.0;
		}
		else if (_settled_since < 0.0)
		{
			_settled_since = now();
		}

		if (_settled_since >= 0.0 && now() - _settled_since >= settle_quiet_seconds) return true;

		if (elapsed >= limit)
		{
			const auto where = step.line > 0 ? std::format("line {}", step.line) : std::string("the scene");
			say(std::format("{}: still busy after {:.0f} s; going on", where, limit));
			return true;
		}

		return false;
	}

	case tutorial_step_kind::type:
	{
		const auto& text = step.text;
		const auto due = static_cast<size_t>(elapsed * typing_characters_per_second) + 1;
		size_t end = 0;

		for (size_t typed = 0; end < text.size() && typed < due; ++typed)
		{
			++end;
			while (end < text.size() && (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80) ++end;
		}

		if (end != _typed)
		{
			_typed = end;
			const auto typed = std::string_view(text).substr(0, end);
			_app._search_edit->window_text(typed);
			_app.search_text_changed(typed);
		}

		return _typed >= text.size() && elapsed >= _step_seconds;
	}

	case tutorial_step_kind::drag:
		if (elapsed < _step_seconds) return false;

		if (_dragging)
		{
			_dragging = false;
			send_pointer_input(ui::pointer_input::move, platform::pointer_button::left, step.keys, _pointer_to);
			send_pointer_input(ui::pointer_input::up, platform::pointer_button::left, step.keys, _pointer_to);
		}

		return true;

	default: return elapsed >= _step_seconds;
	}
}

bool tutorial_player::is_settled()
{
	const auto& index = _app._item_index;
	return index.is_init_complete() && index.scanning_items == 0 && index.searching == 0 && index.indexing == 0 &&
	       index.thumbnailing_items == 0 && df::loading_media == 0;
}

std::optional<pointi> tutorial_player::anchor_point(const tutorial_anchor& anchor)
{
	auto& app = _app;
	const auto& view_frame = app._view_frame;
	const auto& items = app._view_items;
	if (!view_frame || !items) return {};

	const auto view_origin = view_frame->frame()->client_to_screen({});
	const auto in_view = [&](const recti bounds) -> std::optional<pointi>
	{
		if (bounds.is_empty()) return {};
		return bounds.offset(view_origin).center();
	};
	const auto browsing = app._state.view_mode() == view_type::items;
	const auto& sidebar = items->sidebar();
	const auto in_sidebar = [&](const view_element_ptr& e) -> std::optional<pointi>
	{
		if (!browsing || !sidebar || !e || !e->is_visible() || !items->sidebar_visible()) return {};
		return in_view(e->bounds.offset(-sidebar->_scroller.scroll_offset()).intersection(items->sidebar_bounds()));
	};

	switch (anchor.kind)
	{
	case tutorial_anchor_kind::address:
	{
		const auto bounds = app._search_edit ? app._search_edit->window_bounds() : recti{};
		if (bounds.is_empty() || !app._show_bars) return {};
		// Into the text, where a click to type lands, rather than its middle.
		return pointi{bounds.left + std::min(bounds.width() / 4, bounds.height() * 4), bounds.center().y};
	}

	case tutorial_anchor_kind::sidebar:
		if (!browsing || !items->sidebar_visible()) return {};
		return in_view(items->sidebar_bounds());

	case tutorial_anchor_kind::types: return sidebar ? in_sidebar(sidebar->_type_chart) : std::nullopt;

	case tutorial_anchor_kind::map: return sidebar ? in_sidebar(sidebar->_map) : std::nullopt;

	case tutorial_anchor_kind::calendar: return sidebar ? in_sidebar(sidebar->_history_chart) : std::nullopt;

	case tutorial_anchor_kind::month:
	{
		const auto chart = sidebar ? sidebar->_history_chart : nullptr;
		if (!chart || !in_sidebar(chart)) return {};

		// As a user would scroll the calendar to the year first.
		chart->show_year(anchor.year);

		const auto months = chart->keyboard_months();
		const auto shown = std::ranges::find(months, std::pair{anchor.year, anchor.month - 1});
		if (shown == months.end()) return {};

		return in_view(chart->month_bounds(chart->bounds, anchor.year, anchor.month - 1)
		                   .offset(-sidebar->_scroller.scroll_offset()));
	}

	case tutorial_anchor_kind::items: return browsing ? in_view(items->calc_items_bounds()) : std::nullopt;

	case tutorial_anchor_kind::preview: return browsing ? in_view(items->calc_media_bounds()) : std::nullopt;

	case tutorial_anchor_kind::view: return in_view(recti(view_frame->_extent));

	case tutorial_anchor_kind::button:
	{
		const auto command = app.find_command(anchor.command);
		if (!command) return {};

		for (const auto& bar : {app._title_bar, app._status_bar})
		{
			for (const auto& control : bar->_controls)
			{
				const auto toolbar = std::dynamic_pointer_cast<ui::toolbar>(control);
				const auto bounds = toolbar ? toolbar->button_bounds(command) : recti{};
				if (!bounds.is_empty()) return bounds.center();
			}
		}

		return {};
	}

	case tutorial_anchor_kind::tile:
	{
		const auto item = browsing ? app._state.find_displayed_item_by_name(anchor.name) : nullptr;
		if (!item) return {};

		items->make_visible(item);
		const auto bounds = item->bounds.offset(-items->_items_scroller.scroll_offset())
		                        .intersection(items->calc_items_bounds());
		if (bounds.is_empty()) return {};

		const auto on_screen = bounds.offset(view_origin);
		return pointi{on_screen.left + static_cast<int>(std::lround(on_screen.width() * anchor.x)),
		              on_screen.top + static_cast<int>(std::lround(on_screen.height() * anchor.y))};
	}

	case tutorial_anchor_kind::at:
	{
		const auto extent = app._extent;
		return app._app_frame->client_to_screen(
			{static_cast<int>(std::lround(extent.cx * anchor.x)), static_cast<int>(std::lround(extent.cy * anchor.y))});
	}
	}

	return {};
}

void tutorial_player::move_pointer(const std::optional<pointi> to, const double seconds)
{
	if (to)
	{
		_pointer_from = _pointer_shown ? pointer_now() : *to;
		_pointer_to = *to;
		_pointer_moved = now();
		_pointer_seconds = _pointer_shown ? seconds : 0.0;
		_pointer_shown = true;
	}
	else
	{
		_pointer_shown = false;
	}

	if (_app._recording) _app._recording->pointer(to, seconds);
}

// Where the pointer is now, eased as the recorder draws it.
pointi tutorial_player::pointer_now() const
{
	const auto t = now() - _pointer_moved;
	if (_pointer_seconds <= 0.0 || t >= _pointer_seconds) return _pointer_to;

	const auto u = std::max(0.0, t / _pointer_seconds);
	const auto eased = u * u * (3.0 - 2.0 * u);
	return {_pointer_from.x + static_cast<int>(std::lround((_pointer_to.x - _pointer_from.x) * eased)),
	        _pointer_from.y + static_cast<int>(std::lround((_pointer_to.y - _pointer_from.y) * eased))};
}

// The view hears the pointer move over it, as from the mouse: what is under it is marked, a video's frame follows
// it, and what it leaves lets go. A drag gives its moves to the window that took its press, wherever it goes.
void tutorial_player::hover()
{
	if (_dragging)
	{
		if (const auto at = pointer_now(); at != _dragged_to)
		{
			send_pointer_input(ui::pointer_input::move, platform::pointer_button::left, {}, at);
			_dragged_to = at;
		}

		return;
	}

	const auto& view_frame = _app._view_frame;
	if (!view_frame) return;

	std::optional<pointi> local;

	if (_pointer_shown)
	{
		const auto origin = view_frame->frame()->client_to_screen({});
		const auto at = pointer_now();
		const pointi p{at.x - origin.x, at.y - origin.y};
		if (recti(view_frame->_extent).contains(p)) local = p;
	}

	if (local == _hovered) return;

	if (local) view_frame->on_mouse_move(*local, false);
	else if (_hovered) view_frame->on_mouse_leave(*_hovered);
	_hovered = local;
}

void tutorial_player::click(const bool twice, const platform::key_state keys)
{
	auto& app = _app;
	if (app._recording && _pointer_shown) app._recording->click();

	if (!_pointed)
	{
		++_outcome.missed;
		say("a click with the pointer on nothing does nothing");
		return;
	}

	// As the window's own input: the press and release, and a second for a double-click, reach whatever is under the
	// pointer - a control, a toolbar's button, a link or the view - as a user's do. The release looks again at where the
	// system's pointer is; this one is still here.
	const auto left = platform::pointer_button::left;

	if (send_pointer_input(ui::pointer_input::move, left, keys, _pointer_to))
	{
		send_pointer_input(ui::pointer_input::down, left, keys, _pointer_to);
		send_pointer_input(ui::pointer_input::up, left, keys, _pointer_to);

		if (twice)
		{
			send_pointer_input(ui::pointer_input::double_click, left, keys, _pointer_to);
			send_pointer_input(ui::pointer_input::up, left, keys, _pointer_to);
		}

		send_pointer_input(ui::pointer_input::move, left, keys, _pointer_to);
		return;
	}

	// A window that takes no input of its own is clicked through the application's handlers.
	switch (_pointed->kind)
	{
	case tutorial_anchor_kind::address: app._search_edit->focus(); return;

	case tutorial_anchor_kind::month:
		if (const auto& sidebar = app._view_items->sidebar())
		{
			sidebar->_history_chart->open_month(app._view_frame, _pointed->year, _pointed->month - 1);
		}
		return;

	case tutorial_anchor_kind::button: app.invoke(_pointed->command); return;

	default: break;
	}

	// Anywhere in the view: the press and release, and a second for a double-click, as the platform passes them on.
	const auto& view_frame = app._view_frame;
	const auto origin = view_frame->frame()->client_to_screen({});
	const pointi p{_pointer_to.x - origin.x, _pointer_to.y - origin.y};

	if (!recti(view_frame->_extent).contains(p))
	{
		++_outcome.missed;
		say("a click outside the view does nothing");
		return;
	}

	// A press on the view gives it the keyboard.
	if (!app._view_has_focus) app.focus_view();

	view_frame->on_mouse_move(p, false);
	view_frame->on_mouse_left_button_down(p, keys);
	view_frame->on_mouse_left_button_up(p, keys);

	if (twice)
	{
		view_frame->on_mouse_left_button_double_click(p, keys);
		view_frame->on_mouse_left_button_up(p, keys);
	}

	// The release looks again at where the system's pointer is; this one is still here.
	view_frame->on_mouse_move(p, false);
	_hovered = p;
}

bool tutorial_player::send_pointer_input(const ui::pointer_input input, const platform::pointer_button button,
                                         const platform::key_state keys, const pointi at)
{
	const auto& frame = _app._app_frame;
	return frame && ui::send_pointer(frame, input, button, at - frame->client_to_screen({}), keys);
}

void tutorial_player::right_click(const platform::key_state keys)
{
	auto& app = _app;
	if (app._recording && _pointer_shown) app._recording->click();

	if (!_pointed || !send_pointer_input(ui::pointer_input::move, platform::pointer_button::left, keys, _pointer_to))
	{
		++_outcome.missed;
		say(_pointed ? "the window took no right-click" : "a right-click with the pointer on nothing does nothing");
		return;
	}

	// As the platform raises a context menu once the right button is let go.
	send_pointer_input(ui::pointer_input::down, platform::pointer_button::right, keys, _pointer_to);
	send_pointer_input(ui::pointer_input::up, platform::pointer_button::right, keys, _pointer_to);
	ui::send_context_menu(app._app_frame, _pointer_to - app._app_frame->client_to_screen({}));
}

void tutorial_player::wheel(const int notches)
{
	const auto& frame = _app._app_frame;

	if (!_pointed || !frame ||
	    !ui::send_wheel(frame, _pointer_to - frame->client_to_screen({}), notches * wheel_detent))
	{
		++_outcome.missed;
		say(_pointed ? "the window took no turn of the wheel"
		             : "a wheel turned with the pointer on nothing does nothing");
	}
}

void tutorial_player::key(const char32_t key, const platform::key_state keys)
{
	auto& app = _app;
	if (!app._search_has_focus && !app._focused_filter && !app._view_has_focus && !app._view_controls_have_focus)
		app.focus_view();

	// As the keyboard's own: to whatever has it, the application's shortcuts first, as a user's keys go.
	if (!ui::send_key(app._app_frame, key, keys)) app.key_down(key, keys);
}

void tutorial_player::finish(const std::string_view outcome, const bool ok)
{
	if (_stage == stage::finished) return;
	_stage = stage::finished;

	// A drag cut short lets go of what it held.
	if (_dragging)
	{
		_dragging = false;
		send_pointer_input(ui::pointer_input::up, platform::pointer_button::left, {}, pointer_now());
	}

	say(outcome);
	_outcome.ok = ok;
	_outcome.summary = outcome;
	move_pointer({}, 0.0);

	// Only the recording it started: steps acted out while an agent records are drawn into its recording, which goes on.
	if (_owns_recording && _app._recording)
	{
		say(_app.finish_recording());
		_outcome.markers = screen_recording_markers_path(_options.record);
	}

	if (_options.finished) _options.finished(_outcome);
}

void tutorial_player::cancel()
{
	finish("Stopped: the call was cancelled", false);
}
