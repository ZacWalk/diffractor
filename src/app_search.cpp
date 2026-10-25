// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The address box. Search editing, focus, Enter, Cancel and accepting a prediction, and
// the auto-complete that ranks history, recent folders, tags, index vocabulary and scoped values
// (tag:, with:, ...) on the auto-complete worker before publishing them to the prediction list.

#include "pch.h"

#include "app_text.h"

#include "model_location.h"
#include "model_index.h"
#include "model.h"
#include "model_db.h"

#include "ui_dialog.h"

#include "app.h"
#include "view_items.h"

#include <utility>

#include "app_match.h"


class search_auto_complete final : public std::enable_shared_from_this<search_auto_complete>,
                                   public ui::complete_strategy_t
{
	view_state& _state;
	// Writes accepted text back into the native address bar.
	std::function<void(std::string)> _set_text;
	df::string_counts _known;
	ui::auto_complete_match_ptr _selected;
	std::atomic_uint64_t _search_generation = 0;

public:
	std::string no_results_message() override
	{
		return std::string(tt.type_to_search);
	}

	void clear() override
	{
		++_search_generation;
		_known.clear();
		_selected.reset();
	}

	search_auto_complete(view_state& s, std::function<void(std::string)> set_text) :
		_state(s), _set_text(std::move(set_text))
	{
		resize_to_show_results = true;
		auto_select_first = true;
		max_predictions = 25u;
	}

	void initialise(std::function<void(const ui::auto_complete_results&)> complete) override
	{
		_selected.reset();
		_known.clear();
		_state.history.count_strings(_known, 1);
		_state.recent_folders.count_strings(_known, 1);
		_state.recent_tags.count_strings(_known, 1, "#");
		_state.recent_tags.count_strings(_known, 1);
		if (!setting.write_folder.empty()) ++_known[str::cache(setting.write_folder)];

		for (const auto& ks : prop::key_scopes())
		{
			++_known[str::cache(std::format("with:{}", ks.scope))];
			++_known[str::cache(std::format("without:{}", ks.scope))];
		}
	}

	static int calc_auto_complete_word_weight(const index_state::auto_complete_word& word,
	                                          const df::string_counts& known)
	{
		auto result = 1 + std::min(word.occurrences, 10);
		const auto found = known.find(word.text);
		if (found != known.end()) result += found->second;
		return result;
	}

	static int calc_auto_complete_word_weight(const index_state::auto_complete_folder& path,
	                                          const std::string_view query, const df::string_counts& known)
	{
		auto result = 1;
		const auto found = known.find(path.path.text());
		if (found != known.end()) result += found->second;
		if (str::icmp(path.path.name().sv(), query) == 0) result += 5;
		return result;
	}

	// Issue #157: complete the value part of a scoped token (tag: / # / @ / with: / without:)
	// from the matching vocabulary, so e.g. "tag:do" suggests tags starting with "do" and
	// "with:ex" suggests property scopes. Runs on the auto_complete worker thread. Returns
	// true if it produced scope-specific suggestions (which are ranked above generic matches).
	static constexpr int scope_weight = 20;
	static constexpr int literal_query_weight = 1000;

	bool add_scope_completions(const std::string& query, ui::auto_complete_results& found)
	{
		const auto scope = df::classify_search_scope(query);

		if (scope.kind == df::search_scope_kind::none) return false;
		if (scope.kind == df::search_scope_kind::tag && !scope.lead.empty())
		{
			std::vector<std::string> context_tags;
			for (const auto part : str::split(scope.lead, true))
			{
				if (part.size() > 1 && part.front() == '#') context_tags.emplace_back(part.substr(1));
			}

			for (const auto& word : _state.item_index.auto_complete_tag_companions(context_tags, scope.value,
				     max_predictions))
			{
				found.emplace_back(std::make_shared<text_match>(*this, word.text, scope.lead, word.highlights,
				                                                scope_weight + std::min(word.occurrences, 20),
				                                                icon_index::tag));
			}
		}

		if (scope.kind == df::search_scope_kind::with || scope.kind == df::search_scope_kind::without)
		{
			// with: / without: -> property scope names
			for (const auto& ks : prop::key_scopes())
			{
				if (found.size() >= max_predictions) break;
				if (scope.value.empty() || str::starts(ks.scope, scope.value))
				{
					found.emplace_back(std::make_shared<text_match>(*this, scope.format(ks.scope), scope.lead,
					                                                ui::match_highlights{}, scope_weight,
					                                                search_icon(scope.format(ks.scope))));
				}
			}
		}
		else if (scope.kind == df::search_scope_kind::location)
		{
			// locations.md 3.4: a location scope completes from the gazetteer, and the candidate
			// is already the canonical term so committing it runs the search it displays.
			for (const auto& loc : _state.item_index.auto_complete_locations(scope.value, max_predictions,
			                                                                 scope.level))
			{
				if (found.size() >= max_predictions) break;
				found.emplace_back(std::make_shared<text_match>(*this, scope.format(loc.text), scope.lead,
				                                                loc.highlights,
				                                                scope_weight + std::min(loc.occurrences, 20),
				                                                icon_index::location));
			}
		}
		else
		{
			// tag: / # -> tag names ("#tag"); @ -> media groups ("@group")
			for (const auto& w : _state.item_index.auto_complete_words(scope.vocab_query(), max_predictions))
			{
				if (found.size() >= max_predictions) break;
				found.emplace_back(std::make_shared<text_match>(*this, scope.format(w.text), scope.lead, w.highlights,
				                                                scope_weight,
				                                                scope.kind == df::search_scope_kind::tag
					                                                ? icon_index::tag
					                                                : search_icon(scope.format(w.text))));
			}
		}

		return !found.empty();
	}

	void search(const std::string& query, std::function<void(const ui::auto_complete_results&)> complete) override
	{
		const auto search_generation = ++_search_generation;
		const auto known = std::make_shared<const df::string_counts>(_known);

		// The active search and the recent-search list are UI-thread owned and are mutated by
		// view_state::open while a prediction is still running, so snapshot them here.
		const auto current_search_text = _state.search().text();
		auto recents_snapshot = _state.recent_searches.items();
		std::ranges::reverse(recents_snapshot);
		const auto recents = std::make_shared<const std::vector<std::string>>(std::move(recents_snapshot));

		_state._async.queue_async(async_queue::auto_complete,
		                          [this, keep_alive = shared_from_this(), known, recents, current_search_text, query,
			                          complete, search_generation]
		                          {
			                          ui::auto_complete_results found;

			                          const auto query_parts = str::split(query, true);
			                          const auto trimmed_query = str::trim(query);
			                          bool scoped = false;

			                          if (!query.empty())
			                          {
				                          found.emplace_back(std::make_shared<text_match>(*this, query, std::string{},
					                          ui::match_highlights{}, literal_query_weight,
					                          search_icon(query)));
			                          }

			                          if (query_parts.empty() || str::icmp(query, current_search_text) == 0)
			                          {
				                          // Recent searches are arbitrary query text, so only the ones that really are
				                          // paths may be presented (and normalized) as folders.
				                          for (const auto& recent : *recents)
				                          {
					                          // Only a folder that is there is presented as a folder; one that has gone
					                          // stays a recent query. A network location is not checked: confirming
					                          // it could wait out a dropped share with every prediction queued behind.
					                          const auto folder_is_offered = df::is_path(recent) &&
						                          (platform::is_network_location(df::folder_path(recent)) ||
							                          df::folder_path(recent).exists());

					                          if (folder_is_offered)
					                          {
						                          found.emplace_back(
							                          std::make_shared<folder_match>(*this, df::folder_path(recent)));
					                          }
					                          else
					                          {
						                          found.emplace_back(std::make_shared<text_match>(
							                          *this, recent, std::string{},
							                          ui::match_highlights{}, 1,
							                          search_icon(recent)));
					                          }
				                          }
			                          }
			                          else
			                          {
				                          scoped = add_scope_completions(query, found);

				                          if (found.size() < max_predictions && df::is_path(str::trim(query)))
				                          {
					                          df::folder_path folder(query);

					                          if (!folder.exists())
					                          {
						                          folder = folder.parent();
					                          }

					                          const df::item_selector selector(folder);

					                          for (const auto& fi : platform::select_folders(
						                               selector, setting.show_hidden))
					                          {
						                          ui::match_highlights m;
						                          auto path = folder.combine(fi.name);

						                          if (find_auto_complete(query_parts, path.text(), true, m))
						                          {
							                          found.emplace_back(
								                          std::make_shared<folder_match>(*this, path, m, 10));
						                          }
					                          }
				                          }

				                          if (!scoped && found.size() < max_predictions)
				                          {
					                          const auto found_folders = _state.item_index.auto_complete_folders(
						                          query, 6);

					                          for (const auto& path : found_folders)
					                          {
						                          found.emplace_back(std::make_shared<folder_match>(
							                          *this, path.path, path.highlights,
							                          calc_auto_complete_word_weight(path, query, *known)));
					                          }
				                          }

				                          if (!scoped && found.size() < max_predictions)
				                          {
					                          const auto found_words = _state.item_index.auto_complete_words(query, 8);

					                          for (const auto& word : found_words)
					                          {
						                          found.emplace_back(std::make_shared<text_match>(
							                          *this, word.text, std::string{},
							                          word.highlights,
							                          calc_auto_complete_word_weight(word, *known)));
					                          }
				                          }

				                          if (!scoped && query_parts.size() == 1 && !trimmed_query.empty() &&
					                          trimmed_query.front() != '#' && trimmed_query.front() != '@')
				                          {
					                          for (const auto& word : _state.item_index.auto_complete_words(
						                               std::format("#{}", trimmed_query), 5))
					                          {
						                          found.emplace_back(std::make_shared<text_match>(
							                          *this, word.text, std::string{},
							                          word.highlights,
							                          15 + std::min(word.occurrences, 10), icon_index::tag));
					                          }
				                          }

				                          // locations.md 3.5: a bare place name is the most guessable spelling and the most
				                          // misleading one, because a plain text search only sees stored place fields. The
				                          // completion offers the resolved `loc:` term instead, and a multi-word name such as
				                          // "new york" has to be offered too or the vocabulary is unreachable for half the
				                          // places that need it.
				                          if (!scoped && !trimmed_query.empty() &&
					                          trimmed_query.front() != '#' && trimmed_query.front() != '@' &&
					                          !df::is_path(trimmed_query))
				                          {
					                          for (const auto& location : _state.item_index.auto_complete_locations(
						                               trimmed_query, 5))
					                          {
						                          found.emplace_back(std::make_shared<text_match>(
							                          *this, location.text, std::string{},
							                          location.highlights,
							                          12 + std::min(location.occurrences, 10),
							                          icon_index::location));
					                          }
				                          }
			                          }

			                          if (!scoped && found.size() < max_predictions && query_parts.size() > 1)
			                          {
				                          const auto query_back = query_parts.back();
				                          const auto lead_text = query.substr(0, query.rfind(query_back));
				                          const auto found_words = _state.item_index.auto_complete_words(
					                          query_back, max_predictions);

				                          for (const auto& word : found_words)
				                          {
					                          found.emplace_back(std::make_shared<text_match>(
						                          *this, word.text, lead_text, word.highlights,
						                          calc_auto_complete_word_weight(word, *known)));
				                          }

				                          if (found.size() < max_predictions)
				                          {
					                          const auto found_folders = _state.item_index.auto_complete_folders(
						                          query_back, max_predictions);

					                          for (const auto& path : found_folders)
					                          {
						                          found.emplace_back(std::make_shared<folder_match>(
							                          *this, path.path, lead_text, path.highlights,
							                          calc_auto_complete_word_weight(path, query, *known)));
					                          }
				                          }
			                          }

			                          if (found.size() < max_predictions && !query.empty() && str::is_white_space(
				                          query.back()))
			                          {
				                          std::vector<std::string> context_tags;
				                          for (const auto part : query_parts)
				                          {
					                          if (part.size() > 1 && part.front() == '#') context_tags.emplace_back(
						                          part.substr(1));
				                          }

				                          for (const auto& word : _state.item_index.auto_complete_tag_companions(
					                               context_tags, {}, 8))
				                          {
					                          found.emplace_back(std::make_shared<text_match>(
						                          *this, word.text, query, word.highlights,
						                          30 + std::min(word.occurrences, 20), icon_index::tag));
				                          }

				                          if (found.size() < max_predictions)
				                          {
					                          for (const auto& word : *known)
					                          {
						                          found.emplace_back(std::make_shared<text_match>(
							                          *this, std::string(word.first), query,
							                          ui::match_highlights{}, word.second));
					                          }
				                          }

				                          if (found.size() < max_predictions)
				                          {
					                          const auto found_groups = _state.item_index.auto_complete_words(
						                          "@", max_predictions);

					                          for (const auto& word : found_groups)
					                          {
						                          found.emplace_back(std::make_shared<text_match>(
							                          *this, word.text, query, word.highlights,
							                          calc_auto_complete_word_weight(word, *known)));
					                          }
				                          }

				                          if (found.size() < max_predictions)
				                          {
					                          const auto found_tags = _state.item_index.auto_complete_words(
						                          "#", max_predictions);

					                          for (const auto& word : found_tags)
					                          {
						                          found.emplace_back(std::make_shared<text_match>(
							                          *this, word.text, query, word.highlights,
							                          calc_auto_complete_word_weight(word, *known)));
					                          }
				                          }
			                          }

			                          if (found.size() < max_predictions)
			                          {
				                          for (const auto& k : *known)
				                          {
					                          if (str::starts(k.first, trimmed_query) &&
						                          found.size() < max_predictions)
					                          {
						                          found.emplace_back(
							                          std::make_shared<text_match>(
								                          *this, std::string(k.first), std::string{}));
					                          }
				                          }
			                          }

			                          // Rank higher-weight suggestions first (literal query, contextual/scoped values, exact/recent matches)
			                          // while preserving discovery order for equal weights.
			                          std::ranges::stable_sort(found, [](const auto& a, const auto& b)
			                          {
				                          return a->weight > b->weight;
			                          });

			                          df::hash_set<std::string, df::ihash, df::ieq> seen;
			                          std::erase_if(found, [&seen](const auto& candidate)
			                          {
				                          return !seen.emplace(candidate->edit_text()).second;
			                          });

			                          if (found.size() > max_predictions) found.resize(max_predictions);
			                          _state.queue_ui([keep_alive, complete, found, search_generation]
			                          {
				                          if (keep_alive->_search_generation == search_generation) complete(found);
			                          });
		                          });
	}

	void selected(const ui::auto_complete_match_ptr& i, const select_type st) override
	{
		// Clearing must be honoured: otherwise a stale suggestion survives the next keystroke and
		// Enter would run it instead of what the user typed.
		if (!i)
		{
			_selected.reset();
			return;
		}

		auto text = i->edit_text();

		if (st == select_type::arrow)
		{
			_set_text(text);
		}

		if (st == select_type::click || st == select_type::double_click)
		{
			_state.open({}, std::move(text));
			_state._events.focus_view();
		}

		_selected = i;
	}

	ui::auto_complete_match_ptr selected() const override
	{
		return _selected;
	}
};

std::shared_ptr<ui::complete_strategy_t> make_search_auto_complete(view_state& state,
                                                                   std::function<void(std::string)> set_text)
{
	return std::make_shared<search_auto_complete>(state, std::move(set_text));
}

void app_frame::hide_search_predictions()
{
	if (_search_completes && _search_predictions_frame)
	{
		_search_completes->clear();
		_search_predictions_frame->frame()->show(false);
	}
}

void app_frame::focus_search(const bool has_focus)
{
	if (_search_has_focus != has_focus)
	{
		_search_has_focus = has_focus;

		if (!_search_predictions_frame)
		{
			ui::frame_style style;
			style.child = false;
			style.no_focus = true;
			style.colors = {
				ui::style::color::toolbar_background, ui::style::color::view_text,
				ui::style::color::view_selected_background
			};

			_search_predictions_frame = std::make_shared<ui::list_frame>();
			_search_predictions_frame->init(_app_frame, _search_completes, style);
		}

		if (_search_has_focus)
		{
			_search_session.begin(_search_edit->window_text());
			_search_completes->initialise([&p = _search_predictions_frame](const ui::auto_complete_results& results)
			{
				p->show_results(results);
			});
			_search_predictions_frame->selected(nullptr, ui::complete_strategy_t::select_type::init);
			_search_predictions_frame->search({});
			_search_predictions_frame->frame()->window_bounds(calc_search_popup_bounds(), true);
		}
		else
		{
			const auto outcome = _search_session.end();
			if (outcome.set_edit_text) set_search_edit_text(outcome.edit_text);
			hide_search_predictions();
		}
	}
}

void app_frame::search_enter()
{
	const auto selected = _search_completes ? _search_completes->selected() : nullptr;
	const auto text = _search_session.commit(selected != nullptr, selected ? selected->edit_text() : std::string_view{},
	                                         _search_edit->window_text());
	_state.open(_view_frame, text);
	focus_view();
}

void app_frame::cancel_search_edit()
{
	const auto outcome = _search_session.escape();

	if (outcome.set_edit_text) set_search_edit_text(outcome.edit_text);

	if (outcome.select_first_completion && _search_predictions_frame)
	{
		const auto& results = _search_predictions_frame->_results;
		_search_predictions_frame->selected(
			results.empty() ? nullptr : results.front(), ui::complete_strategy_t::select_type::init);
	}

	if (outcome.close_popup) hide_search_predictions();
	if (outcome.focus_view) focus_view();
}

bool app_frame::search_accept_selected()
{
	// Tab-completion: fill the address bar with the highlighted suggestion (without running
	// the search) and refresh predictions so the completed token can be extended further.
	const auto sel = _search_completes ? _search_completes->selected() : nullptr;
	const auto outcome = _search_session.accept(sel != nullptr, sel ? sel->edit_text() : std::string_view{});

	if (!outcome.handled) return false;

	if (outcome.set_edit_text) set_search_edit_text(outcome.edit_text);
	if (outcome.refresh_completions && _search_predictions_frame) _search_predictions_frame->search(outcome.edit_text);
	return true;
}

void app_frame::set_search_edit_text(const std::string_view text)
{
	_search_setting_text = true;
	_search_edit->window_text(text);
	_search_setting_text = false;
}

void app_frame::preview_search_prediction(const std::string& text)
{
	_search_session.preview(text);
	set_search_edit_text(text);
}

void app_frame::init_search()
{
	_search_completes = make_search_auto_complete(_state, [this](std::string text)
	{
		preview_search_prediction(std::move(text));
	});
}
