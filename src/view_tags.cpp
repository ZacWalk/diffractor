// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tagging workflow view. Previews the resulting tags for every selected item and
// applies the add/remove actions using the shared run and progress model.

#include "pch.h"
#include "model.h"
#include "model_db.h"
#include "model_index.h"
#include "model_tokenizer.h"
#include "util_top.h"
#include "files.h"
#include "app_command_status.h"
#include "view_tags.h"

#include "ui_controls.h"

std::vector<tag_action> parse_tag_actions(const std::string_view text)
{
	std::vector<tag_action> result;
	auto pos = 0_z;

	const auto skip_separator = [&]
	{
		while (pos < text.size() && str::is_separator(text[pos])) ++pos;
	};

	const auto read_quoted = [&](const char quote_char)
	{
		std::string tag;
		++pos;

		while (pos < text.size())
		{
			const auto c = text[pos++];

			if (c == quote_char)
			{
				if (pos < text.size() && text[pos] == quote_char)
				{
					tag += c;
					++pos;
				}
				else
				{
					break;
				}
			}
			else
			{
				tag += c;
			}
		}

		return tag;
	};

	const auto read_bare = [&]
	{
		const auto start = pos;
		while (pos < text.size() && !str::is_separator(text[pos])) ++pos;
		return std::string(text.substr(start, pos - start));
	};

	while (pos < text.size())
	{
		skip_separator();
		if (pos >= text.size()) break;

		auto positive = true;
		if (text[pos] == '-' || text[pos] == '!')
		{
			positive = false;
			++pos;
			skip_separator();
		}

		if (pos < text.size() && text[pos] == '#') ++pos;
		const auto quoted = pos < text.size() && (text[pos] == '"' || text[pos] == '\'');
		auto tag = quoted ? read_quoted(text[pos]) : read_bare();
		if (tag.empty()) continue;

		const auto found = std::ranges::find_if(result, [&tag](const tag_action& action)
		{
			return str::icmp(action.first, tag) == 0;
		});

		if (found == result.end())
			result.emplace_back(std::move(tag), positive);
		else
			found->second = positive;
	}

	return result;
}

static bool tag_action_needs_quote(const std::string_view tag)
{
	if (tag.empty()) return true;
	if (tag.front() == '-' || tag.front() == '!') return true;
	if (str::icmp(tag, "and") == 0 || str::icmp(tag, "or") == 0 ||
		str::icmp(tag, tt.query_and) == 0 || str::icmp(tag, tt.query_or) == 0)
	{
		return true;
	}

	return std::ranges::any_of(tag, [](const char c)
	{
		return str::is_separator(c) || c == '\'' || c == '"' || search_tokenizer::is_delimiter_char(c);
	});
}

static std::string quote_tag_action_literal(const std::string_view tag)
{
	if (!tag_action_needs_quote(tag)) return std::string(tag);

	const auto quote_char = tag.find('"') == std::string::npos ? '"' : '\'';
	std::string result;
	result += quote_char;

	for (const auto c : tag)
	{
		result += c;
		if (c == quote_char) result += c;
	}

	result += quote_char;
	return result;
}

std::string serialize_tag_actions(const std::vector<tag_action>& actions)
{
	std::vector<std::string> parts;

	for (const auto& [tag, positive] : actions)
	{
		auto text = quote_tag_action_literal(tag);
		parts.emplace_back(positive ? std::move(text) : std::format("-{}", text));
	}

	return str::combine(parts, " ", false);
}

void tags_view::parse_tags()
{
	_adds.clear();
	_removes.clear();

	for (const auto& [tag, positive] : parse_tag_actions(_tags))
	{
		if (positive) _adds.emplace_back(tag);
		else _removes.emplace_back(tag);
	}
}

bool tags_view::contains_action(const std::string_view tag, const bool positive) const
{
	const auto& list = positive ? _adds : _removes;
	return std::ranges::any_of(list, [tag](const std::string& t) { return str::icmp(t, tag) == 0; });
}

void tags_view::toggle_action(const std::string_view tag, const bool positive)
{
	auto actions = parse_tag_actions(_tags);

	const auto found = std::ranges::find_if(actions, [tag](const tag_action& action)
	{
		return str::icmp(action.first, tag) == 0;
	});

	if (found == actions.end())
		actions.emplace_back(tag, positive);
	else if (found->second == positive)
		actions.erase(found);
	else
		found->second = positive;

	_tags = serialize_tag_actions(actions);
	if (_edit) _edit->text(_tags);
	refresh();
	refresh_words();
}

void tags_view::refresh_words() const
{
	if (_word_refresh)
	{
		for (const auto& update : *_word_refresh) update();
	}
}

bool tags_view::can_run() const
{
	// A finished run leaves result rows, which describe what happened rather than what would happen.
	// Running those would report against a plan that no longer exists; editing the tag field or
	// changing the selection routes through refresh() and puts the view back into review.
	return !showing_results() && !_rows.empty() && (!_adds.empty() || !_removes.empty());
}

void tags_view::refresh()
{
	// The worker reports against the reviewed rows, so rebuilding them under it would repoint the
	// run at a different set of files.
	if (progress().active) return;
	// Every input to the plan routes here, so a change to one is also what leaves results mode.
	_showing_results = false;

	const auto& items = _state.selected_items();

	parse_tags();

	const tag_set add_tags(_adds);
	const tag_set remove_tags(_removes);
	const auto unchanged_color = ui::darken(ui::style::color::view_text, 0.22f);
	const auto changed_color = ui::lighten(ui::style::color::dialog_selected_background, 0.55f);

	std::vector<row_element_ptr> rows;
	rows.reserve(items.size());

	auto count = 0;
	auto changes = 0;

	for (const auto& i : items.items())
	{
		const auto md = i->metadata();

		tag_set result = md ? tag_set(md->tags) : tag_set{};
		const auto before = result.to_string();

		// Mirror the write order used when the edits are applied to the file.
		result.remove(remove_tags);
		result.add(add_tags);
		result.make_unique();

		const auto after = result.to_string();
		const auto changed = before != after;

		auto row = std::make_shared<row_element>(*this);
		row->_text[0] = std::string(i->name());
		row->_text[2] = changed ? after : std::string(tt.tags_unchanged.sv());
		row->_text_color[2] = changed ? changed_color : unchanged_color;
		if (changed) row->_icons[1] = icon_index::next;
		row->_order = count++;
		row->_work_index = row->_order;
		rows.emplace_back(row);

		if (changed) ++changes;
	}

	_rows = std::move(rows);

	if (_adds.empty() && _removes.empty())
	{
		_status = std::string(tt.tags_nothing_to_do.sv());
	}
	else
	{
		// Count of rows the run would change, matching the "{count} {label}" status used by the
		// other guided tasks. tag_selected is a command label, not a count noun.
		_status = std::format("{} {}", changes, tt.changes);
	}

	// The tag field is what decides whether the run button answers, so editing it must refresh
	// command state; otherwise the button waits for an unrelated event such as a selection change.
	_state.invalidate_view(view_invalid::view_layout | view_invalid::controller | view_invalid::status |
		view_invalid::command_state);
}

void tags_view::run()
{
	if (!can_run() || progress().active) return;

	record_feature_use(features::tag);

	const auto& items = _state.selected_items();
	const auto item_list = items.items();
	const auto adds = _adds;

	begin_processing(item_list.size());
	const auto processing_generation = this->processing_generation();
	const auto cancel_source = processing_cancel_source();
	const auto review_status = _status;
	_status = std::string(tt.processing.sv());

	const auto results = make_run_status(shared_from_this(), processing_generation, cancel_source, review_status);

	metadata_edits edits;
	edits.add_tags = tag_set(_adds);
	edits.remove_tags = tag_set(_removes);

	_state.modify_items(results, icon_index::tag, tt.tag_add_remove, item_list, edits, _host);
	_state.recent_tags.add_items(adds);
}

view_controls_host_ptr tags_view::controls(const ui::control_frame_ptr& owner)
{
	auto result = std::make_shared<view_controls_host>(_state);
	auto frame = owner->create_dlg(result, false);

	const auto& items = _state.selected_items();
	auto selection_thumbnails = std::make_shared<ui::selection_thumbnails_control>(frame);
	selection_thumbnails->selection_async(items.thumbs(), items.size(), _state._async);

	constexpr size_t recommended_tag_limit = 20;

	_word_refresh = std::make_shared<ui::recommended_words_control::refresh_group::element_type>();

	const auto add_tag = [this](const std::string_view tag) { toggle_action(tag, true); };
	const auto remove_tag = [this](const std::string_view tag) { toggle_action(tag, false); };
	const auto is_add = [this](const std::string_view tag) { return contains_action(tag, true); };
	const auto is_remove = [this](const std::string_view tag) { return contains_action(tag, false); };

	std::vector<view_element_ptr> controls;
	controls.emplace_back(create_view_info_element(tt.tag_info));
	controls.emplace_back(selection_thumbnails);
	controls.emplace_back(std::make_shared<text_element>(format_plural_text(tt.tag_info_fmt, items)));
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(std::make_shared<text_element>(tt.tag_add_or_remove_label));
	controls.emplace_back(_edit = std::make_shared<ui::multi_line_edit_control>(frame, _tags, 6, false,
		[this](const std::string_view)
		{
			refresh();
			refresh_words();
		}));
	controls.emplace_back(std::make_shared<text_element>(tt.help_tag1));
	controls.emplace_back(std::make_shared<text_element>(tt.help_tag2));
	controls.emplace_back(std::make_shared<text_element>(tt.help_tag_add_remove));

	// Interned, not views into setting.favorite_tags: that string is reassigned by the
	// options and favourite-tag commands, which do not rebuild these controls.
	_favorite_words.clear();

	for (const auto& part : str::split(setting.favorite_tags, true))
	{
		_favorite_words.emplace_back(str::cache(part).sv());
	}

	if (_favorite_words.size() > recommended_tag_limit)
	{
		_favorite_words.resize(recommended_tag_limit);
	}

	if (!_favorite_words.empty())
	{
		controls.emplace_back(std::make_shared<divider_element>());
		controls.emplace_back(std::make_shared<text_element>(
			tt.tags_favorite_label, ui::style::font_face::title, ui::style::text_style::multiline,
			view_element_options{}));
		controls.emplace_back(std::make_shared<ui::recommended_words_control>(
			frame, _favorite_words, add_tag, is_add, _word_refresh));
	}

	controls.emplace_back(std::make_shared<link_element>(tt.configure_favorite_tags, commands::favorite_tags));

	_common_counts.clear();

	for (const auto& t : _state.item_index.distinct_tags())
	{
		++_common_counts[t.first];
	}

	_state.recent_tags.count_strings(_common_counts, 1000000);

	for (const auto& f : _favorite_words)
	{
		_common_counts.erase(f);
	}

	_common_words = top_map(_common_counts, recommended_tag_limit);

	if (!_common_words.empty())
	{
		controls.emplace_back(std::make_shared<divider_element>());
		controls.emplace_back(std::make_shared<text_element>(
			tt.tags_common_label, ui::style::font_face::title, ui::style::text_style::multiline,
			view_element_options{}));
		controls.emplace_back(std::make_shared<ui::recommended_words_control>(
			frame, _common_words, add_tag, is_add, _word_refresh));
	}

	_existing_counts = _state.selected_tags();
	_existing_words = top_map(_existing_counts, recommended_tag_limit);

	if (!_existing_words.empty())
	{
		controls.emplace_back(std::make_shared<divider_element>());
		controls.emplace_back(std::make_shared<text_element>(
			tt.tags_remove_label, ui::style::font_face::title, ui::style::text_style::multiline,
			view_element_options{}));
		controls.emplace_back(std::make_shared<ui::recommended_words_control>(
			frame, _existing_words, remove_tag, is_remove, _word_refresh));
	}

	for (const auto& c : controls)
	{
		c->margin.cx = 8;
		c->margin.cy = 4;
	}

	result->_controls = controls;
	result->_frame = result->_dlg = frame;
	// The view exists to have tags typed into it, so it opens ready for typing.
	result->initial_focus = [edit = _edit] { edit->focus(); };
	return result;
}
