// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The address box. Search editing, focus, Enter, Cancel and accepting a prediction, and
// the auto-complete that ranks history, recent folders, tags, index vocabulary and scoped values
// (tag:, with:, ...) on the auto-complete worker, counting what each finds through the search index,
// before publishing them to the prediction list. Matching items supply next-term refinements;
// person rows can receive a cached thumbnail crop after publication.

#include "pch.h"

#include "app_text.h"

#include "model_location.h"
#include "model_index.h"
#include "model_faces_search.h"
#include "model.h"
#include "model_db.h"

#include "ui_dialog.h"

#include "app.h"
#include "view_items.h"

#include <utility>

#include "app_match.h"

class search_auto_complete final :
	public std::enable_shared_from_this<search_auto_complete>,
	public ui::complete_strategy_t
{
	view_state& _state;
	// Writes accepted text back into the native address bar.
	std::function<void(std::string)> _set_text;
	std::function<void()> _repaint;
	std::weak_ptr<const faces::overview> _portrait_overview;
	core::hash_map<uint32_t, core::const_surface_ptr> _portraits;
	core::string_counts _known;
	ui::auto_complete_match_ptr _selected;
	std::atomic_uint64_t _search_generation = 0;
	// Raised by each keystroke on the UI thread, which stops the counting a superseded pass is doing.
	std::shared_ptr<std::atomic_int> _count_version = std::make_shared<std::atomic_int>(0);

	// Long enough for the search index to count a keystroke's predictions, short enough that a
	// collection it cannot answer never holds up the list.
	static constexpr int64_t prediction_count_budget_us = 40'000;

public:
	std::string no_results_message() override { return std::string(tt.type_to_search); }

	void clear() override
	{
		++_search_generation;
		++*_count_version;
		_known.clear();
		_selected.reset();
	}

	search_auto_complete(view_state& s, std::function<void(std::string)> set_text, std::function<void()> repaint) :
		_state(s), _set_text(std::move(set_text)), _repaint(std::move(repaint))
	{
		resize_to_show_results = true;
		auto_select_first = true;
		max_predictions = 25u;
	}

	void initialise(std::function<void(const ui::auto_complete_results&)> /*complete*/) override
	{
		_selected.reset();
		_known.clear();
		_state.history.count_strings(_known, 1);
		_state.recent_folders.count_strings(_known, 1);
		_state.recent_tags.count_strings(_known, 1, "#");
		_state.recent_tags.count_strings(_known, 1);
		if (!setting.write_folder.empty()) ++_known[core::cache(setting.write_folder)];

		for (const auto& ks : prop::key_scopes())
		{
			++_known[core::cache(std::format("with:{}", ks.scope))];
			++_known[core::cache(std::format("without:{}", ks.scope))];
		}
	}

	static int calc_auto_complete_word_weight(const index_state::auto_complete_word& word,
	                                          const core::string_counts& known)
	{
		auto result = 1 + std::min(word.occurrences, 10);
		const auto found = known.find(word.text);
		if (found != known.end()) result += found->second;
		return result;
	}

	static int calc_auto_complete_word_weight(const index_state::auto_complete_folder& path,
	                                          const std::string_view query, const core::string_counts& known)
	{
		auto result = 1;
		const auto found = known.find(path.path.text());
		if (found != known.end()) result += found->second;
		if (core::icmp(path.path.name().sv(), query) == 0) result += 5;
		return result;
	}

	// A suggestion drawn from the collection's own words, tags, places and properties, offered only
	// when it finds something.
	std::shared_ptr<text_match> vocabulary_match(std::string text, std::string lead, ui::match_highlights highlights,
	                                             const int weight, const icon_index icon)
	{
		auto result = std::make_shared<text_match>(*this, std::move(text), std::move(lead), std::move(highlights),
		                                           weight, icon);
		result->drop_when_empty = true;
		return result;
	}

	// What choosing a prediction searches for. Read without asking the file system, because a path
	// on a share that has gone away would hold the list until the network gave up.
	static std::optional<df::search_t> prediction_search(const ui::auto_complete_match& match)
	{
		if (const auto* const folder = dynamic_cast<const folder_match*>(&match);
		    folder && core::is_empty(folder->lead))
		{
			df::search_t result;
			result.add_selector(core::item_selector(folder->folder));
			return result;
		}

		auto result = df::search_t::parse_without_probing(match.edit_text());
		if (result.is_empty()) return std::nullopt;
		return result;
	}

	// Ten times as many items is worth the same step in rank wherever it starts.
	static int count_rank(const uint64_t count)
	{
		return static_cast<int>(std::lround(8.0 * std::log10(1.0 + static_cast<double>(count))));
	}

	// Each prediction says how many items it finds. One that finds more ranks above one of its kind that
	// finds fewer, and one drawn from the collection's vocabulary that finds nothing is left out. Only
	// the search index is asked, within a budget, and a newer keystroke stops the pass, so a prediction
	// left uncounted keeps its place rather than waiting on a scan. Answers false for a superseded pass.
	bool count_predictions(ui::auto_complete_results& found, const uint64_t search_generation, const bool rank,
	                       const core::cancel_token& token, search_counter& counter)
	{
		// Likeliest first, so the budget is spent where it decides the most, and each text counted once.
		std::ranges::stable_sort(found, [](const auto& a, const auto& b) { return a->weight > b->weight; });

		core::hash_set<std::string, core::ihash, core::ieq> seen;
		std::erase_if(found, [&seen](const auto& candidate) { return !seen.emplace(candidate->edit_text()).second; });

		const auto deadline = core::now_us() + prediction_count_budget_us;

		for (const auto& match : found)
		{
			if (_search_generation != search_generation || token.is_cancelled()) return false;
			if (core::now_us() > deadline) break;

			if (const auto search = prediction_search(*match))
			{
				match->count = counter.try_count_items(*search, token);
			}
		}

		if (_search_generation != search_generation || token.is_cancelled()) return false;

		std::erase_if(found, [](const auto& match) { return match->drop_when_empty && match->count == 0u; });

		if (rank)
		{
			for (const auto& match : found)
			{
				if (match->count) match->weight += count_rank(*match->count);
			}

			std::ranges::stable_sort(found, [](const auto& a, const auto& b) { return a->weight > b->weight; });
		}

		return true;
	}

	// Issue #157: complete the value part of a scoped token (tag: / # / @ / with: / without:)
	// from the matching vocabulary, so e.g. "tag:do" suggests tags starting with "do" and
	// "with:ex" suggests property scopes. Runs on the auto_complete worker thread. Returns
	// true if it produced scope-specific suggestions (which are ranked above generic matches).
	static constexpr int scope_weight = 20;

	void add_person_completions(const df::search_scope_completion& scope, ui::auto_complete_results& found,
	                            const size_t limit)
	{
		for (const auto& person : _state.item_index.auto_complete_people(scope.value, limit))
		{
			const auto text = scope.format(person.text);
			constexpr auto offset = "person:\""sv.size();
			auto highlights = person.highlights;
			for (auto& h : highlights) h.offset += static_cast<uint32_t>(offset);
			// The face book can know a name before the next overview is ready to count its pictures.
			auto row = std::make_shared<text_match>(*this, text, scope.lead, highlights,
			                                        scope_weight + std::min(person.occurrences, 20), icon_index::person);
			row->person_name = person.text;
			if (!found.empty() && core::icmp(found.front()->edit_text(), row->edit_text()) == 0)
			{
				if (const auto literal = std::dynamic_pointer_cast<text_match>(found.front()))
					literal->person_name = person.text;
			}
			found.emplace_back(std::move(row));
		}
	}
	static constexpr int literal_query_weight = 1000;

	bool add_scope_completions(const df::search_scope_completion& scope, ui::auto_complete_results& found)
	{
		if (scope.kind == df::search_scope_kind::none) return false;
		if (scope.kind == df::search_scope_kind::tag && !scope.lead.empty())
		{
			std::vector<std::string> context_tags;
			for (const auto part : core::split(scope.lead, true))
			{
				if (part.size() > 1 && part.front() == '#') context_tags.emplace_back(part.substr(1));
			}

			for (const auto& word :
			     _state.item_index.auto_complete_tag_companions(context_tags, scope.value, max_predictions))
			{
				found.emplace_back(vocabulary_match(word.text, scope.lead, word.highlights,
				                                    scope_weight + std::min(word.occurrences, 20), icon_index::tag));
			}
		}

		if (scope.kind == df::search_scope_kind::with || scope.kind == df::search_scope_kind::without)
		{
			// with: / without: -> property scope names
			for (const auto& ks : prop::key_scopes())
			{
				if (found.size() >= max_predictions) break;
				if (scope.value.empty() || core::starts(ks.scope, scope.value))
				{
					found.emplace_back(vocabulary_match(scope.format(ks.scope), scope.lead, ui::match_highlights{},
					                                    scope_weight, search_icon(scope.format(ks.scope))));
				}
			}
		}
		else if (scope.kind == df::search_scope_kind::location)
		{
			// docs/locations.md#location-search: a location scope completes from the gazetteer, and the candidate
			// is already the canonical term so committing it runs the search it displays.
			for (const auto& loc : _state.item_index.auto_complete_locations(scope.value, max_predictions, scope.level))
			{
				if (found.size() >= max_predictions) break;
				found.emplace_back(vocabulary_match(scope.format(loc.text), scope.lead, loc.highlights,
				                                    scope_weight + std::min(loc.occurrences, 20),
				                                    icon_index::location));
			}
		}
		else if (scope.kind == df::search_scope_kind::person)
		{
			// docs/faces.md#8-searching: the names the collection holds, most pictures first. The highlights are
			// moved past the scope the committed text leads with.
			add_person_completions(scope, found, found.size() < max_predictions ? max_predictions - found.size() : 0);
		}
		else
		{
			// tag: / # -> tag names ("#tag"); @ -> media groups ("@group")
			for (const auto& w : _state.item_index.auto_complete_words(scope.vocab_query(), max_predictions))
			{
				if (found.size() >= max_predictions) break;
				found.emplace_back(vocabulary_match(scope.format(w.text), scope.lead, w.highlights, scope_weight,
				                                    scope.kind == df::search_scope_kind::tag
				                                        ? icon_index::tag
				                                        : search_icon(scope.format(w.text))));
			}
		}

		return !found.empty();
	}

	struct portrait_job
	{
		uint32_t face;
		core::file_path path;
		rectd area;
		std::weak_ptr<text_match> row;
		core::const_image_ptr thumb;
		core::const_surface_ptr tile;
	};

	// Resolve the representative on the completion worker. The popup is published before any SQLite
	// or decode work, and a missing cached thumbnail never triggers a read of the original picture.
	static std::vector<portrait_job> portrait_jobs(const faces::overview_ptr& overview,
	                                              const ui::auto_complete_results& found)
	{
		std::vector<portrait_job> jobs;
		if (!overview || !overview->store) return jobs;
		for (const auto& match : found)
		{
			const auto row = std::dynamic_pointer_cast<text_match>(match);
			if (!row || row->person_name.empty()) continue;
			const auto* person = overview->find_person(row->person_name);
			if (!person || person->samples.empty()) continue;
			// Cut from a picture's cached thumbnail, which a face found in a video is not on.
			const auto sample = std::ranges::find_if(person->samples, [&overview](const uint32_t f)
			{ return !overview->store->run_of_face(f).video; });
			if (sample == person->samples.end()) continue;
			const auto face = *sample;
			jobs.push_back({face, overview->store->run_of_face(face).path, overview->store->entry(face).bounds(), row,
			                {}, {}});
			if (jobs.size() == 5) break;
		}
		return jobs;
	}

	void load_portraits(const faces::overview_ptr& overview, std::vector<portrait_job> jobs, const uint64_t generation)
	{
		if (overview != _state.item_index.faces().current_overview()) return;
		if (_portrait_overview.lock() != overview)
		{
			_portraits.clear();
			_portrait_overview = overview;
		}
		std::erase_if(jobs, [&](const auto& j)
		{
			const auto cached = _portraits.find(j.face);
			if (cached == _portraits.end()) return false;
			if (const auto row = j.row.lock()) row->portrait = cached->second;
			return true;
		});
		if (jobs.empty()) return;

		auto& async = _state._async;
		async.queue_database([&async, weak = weak_from_this(), version = _count_version,
		                      token = core::cancel_token::current(*_count_version), generation,
		                      jobs = std::move(jobs)](const database& db) mutable
		{
			for (auto& j : jobs)
			{
				if (token.is_cancelled()) return;
				j.thumb = db.load_thumbnail(j.path).thumb;
			}
			async.queue_async(async_queue::thumbnail_decode,
			                  [&async, weak, version, token, generation, jobs = std::move(jobs)]() mutable
			{
				files ff;
				for (auto& j : jobs)
				{
					if (token.is_cancelled()) return;
					if (is_valid(j.thumb))
						j.tile = faces::cut_tile(ff.image_to_surface(j.thumb, {256, 256}, false, token,
						                                            decode_intent::thumbnail), j.area, 40);
				}
				async.queue_ui([weak, generation, jobs = std::move(jobs)]
				{
					const auto self = weak.lock();
					if (!self || self->_search_generation != generation ||
					    self->_portrait_overview.lock() != self->_state.item_index.faces().current_overview()) return;
					bool changed = false;
					for (const auto& j : jobs)
					{
						if (!j.tile) continue;
						if (self->_portraits.size() >= 32) self->_portraits.erase(self->_portraits.begin());
						self->_portraits[j.face] = j.tile;
						if (const auto row = j.row.lock())
						{
							row->portrait = j.tile;
							changed = true;
						}
					}
					if (changed && self->_repaint) self->_repaint();
				});
			});
		});
	}

	void search(const std::string& query, std::function<void(const ui::auto_complete_results&)> complete) override
	{
		const auto search_generation = ++_search_generation;
		const auto known = std::make_shared<const core::string_counts>(_known);
		// Taken here, on the UI thread, so this keystroke stops any counting the last one is still doing.
		const core::cancel_token count_token(*_count_version);

		// The active search and the recent-search list are UI-thread owned and are mutated by
		// view_state::open while a prediction is still running, so snapshot them here.
		const auto current_search_text = _state.search().text();
		auto recents_snapshot = _state.recent_searches.items();
		std::ranges::reverse(recents_snapshot);
		const auto recents = std::make_shared<const std::vector<std::string>>(std::move(recents_snapshot));

		_state._async.queue_async(async_queue::auto_complete,
		                          [this, keep_alive = shared_from_this(), known, recents, current_search_text, query,
		                           complete, search_generation, count_token]
		{
			if (_search_generation != search_generation || count_token.is_cancelled()) return;
			ui::auto_complete_results found;
			search_counter counter(_state.item_index);

			const auto query_parts = core::split(query, true);
			const auto trimmed_query = core::trim(query);
			bool scoped = false;

			if (!query.empty())
			{
				found.emplace_back(std::make_shared<text_match>(*this, query, std::string{}, ui::match_highlights{},
				                                                literal_query_weight, search_icon(query)));
			}

			if (query_parts.empty() || core::icmp(query, current_search_text) == 0)
			{
				// Recent searches are arbitrary query text, so only the ones that really are
				// paths may be presented (and normalized) as folders.
				for (const auto& recent : *recents)
				{
					// Only a folder that is there is presented as a folder; one that has gone
					// stays a recent query. A network location is not checked: confirming
					// it could wait out a dropped share with every prediction queued behind.
					const auto folder_is_offered = core::is_path(recent) &&
					                               (platform::is_network_location(core::folder_path(recent)) ||
					                                platform::exists(core::folder_path(recent)));

					if (folder_is_offered)
					{
						found.emplace_back(std::make_shared<folder_match>(*this, core::folder_path(recent)));
					}
					else
					{
						found.emplace_back(std::make_shared<text_match>(*this, recent, std::string{},
						                                                ui::match_highlights{}, 1,
						                                                search_icon(recent)));
					}
				}
			}
			else
			{
				const auto scope = df::classify_search_scope(query);
				scoped = add_scope_completions(scope, found);
				if (!core::trim(scope.lead).empty() && !core::is_path(trimmed_query))
				{
					const auto base = df::search_t::parse_without_probing(scope.lead);
					for (const auto& word : counter.refinements(base, std::string_view(query).substr(scope.lead.size()),
					                                            count_token))
						found.emplace_back(vocabulary_match(word.text, scope.lead, {}, 40, search_icon(word.text)));
					const auto fragment = std::string_view(query).substr(scope.lead.size());
					if (!scoped && !fragment.empty() && !core::is_white_space(fragment.front()) &&
					    fragment.find_first_of(":-!\"'") == fragment.npos)
					{
						auto person = scope;
						person.kind = df::search_scope_kind::person;
						person.value = fragment;
						add_person_completions(person, found, 4);
					}
				}

				if (found.size() < max_predictions && core::is_path(core::trim(query)))
				{
					core::folder_path folder(query);

					if (!platform::exists(folder))
					{
						folder = folder.parent();
					}

					const core::item_selector selector(folder);

					for (const auto& fi : platform::select_folders(selector, setting.show_hidden))
					{
						ui::match_highlights m;
						auto path = folder.combine(fi.name);

						if (find_auto_complete(query_parts, path.text(), true, m))
						{
							found.emplace_back(std::make_shared<folder_match>(*this, path, m, 10));
						}
					}
				}

				if (!scoped && found.size() < max_predictions)
				{
					const auto found_folders = _state.item_index.auto_complete_folders(query, 6);

					for (const auto& path : found_folders)
					{
						found.emplace_back(
							std::make_shared<folder_match>(*this, path.path, path.highlights,
						                                   calc_auto_complete_word_weight(path, query, *known)));
					}
				}

				if (!scoped && found.size() < max_predictions)
				{
					// More than are shown: counting ranks them, and drops those that lead nowhere.
					const auto found_words = _state.item_index.auto_complete_words(query, 12);

					for (const auto& word : found_words)
					{
						found.emplace_back(vocabulary_match(word.text, std::string{}, word.highlights,
						                                    calc_auto_complete_word_weight(word, *known),
						                                    icon_index::search));
					}
				}

				if (!scoped && query_parts.size() == 1 && !trimmed_query.empty() && trimmed_query.front() != '#' &&
				    trimmed_query.front() != '@')
				{
					for (const auto& word : _state.item_index.auto_complete_words(std::format("#{}", trimmed_query), 8))
					{
						found.emplace_back(vocabulary_match(word.text, std::string{}, word.highlights,
						                                    15 + std::min(word.occurrences, 10), icon_index::tag));
					}

					// A kind is asked for by its `@` word, which a plain word beginning one is offered: "pano" offers
					// @panorama, "hd" @hdr and "commo" @commodore. One letter would offer too many.
					if (trimmed_query.size() >= 2)
					{
						for (const auto& word :
						     _state.item_index.auto_complete_words(std::format("@{}", trimmed_query), 4))
						{
							if (!core::starts(word.text, "@")) continue;
							found.emplace_back(vocabulary_match(word.text, std::string{}, word.highlights,
							                                    15 + std::min(word.occurrences, 10),
							                                    search_icon(word.text)));
						}
					}
				}

				// docs/locations.md#location-search: a bare place name is the most guessable spelling and the most
				// broadest one, because a plain text search also sees names, faces and metadata. The
				// completion offers the narrower `loc:` term too, and a multi-word name such as
				// "new york" has to be offered too or the vocabulary is unreachable for half the
				// places that need it.
				if (!scoped && !trimmed_query.empty() && trimmed_query.front() != '#' && trimmed_query.front() != '@' &&
				    !core::is_path(trimmed_query))
				{
					df::search_scope_completion person;
					person.kind = df::search_scope_kind::person;
					person.value = trimmed_query;
					add_person_completions(person, found, 5);

					for (const auto& location : _state.item_index.auto_complete_locations(trimmed_query, 5))
					{
						found.emplace_back(vocabulary_match(location.text, std::string{}, location.highlights,
						                                    12 + std::min(location.occurrences, 10),
						                                    icon_index::location));
					}
				}
			}

			if (!scoped && found.size() < max_predictions && query_parts.size() > 1)
			{
				const auto query_back = query_parts.back();
				const auto lead_text = query.substr(0, query.rfind(query_back));
				const auto found_words = _state.item_index.auto_complete_words(query_back, max_predictions);

				// Counted with the terms before them, so a word that finds nothing alongside them
				// is not offered whatever it finds alone.
				for (const auto& word : found_words)
				{
					found.emplace_back(vocabulary_match(word.text, lead_text, word.highlights,
					                                    calc_auto_complete_word_weight(word, *known),
					                                    icon_index::search));
				}

				if (found.size() < max_predictions)
				{
					const auto found_folders = _state.item_index.auto_complete_folders(query_back, max_predictions);

					for (const auto& path : found_folders)
					{
						found.emplace_back(
							std::make_shared<folder_match>(*this, path.path, lead_text, path.highlights,
						                                   calc_auto_complete_word_weight(path, query, *known)));
					}
				}
			}

			if (found.size() < max_predictions && !query.empty() && core::is_white_space(query.back()))
			{
				std::vector<std::string> context_tags;
				for (const auto part : query_parts)
				{
					if (part.size() > 1 && part.front() == '#') context_tags.emplace_back(part.substr(1));
				}

				for (const auto& word : _state.item_index.auto_complete_tag_companions(context_tags, {}, 8))
				{
					found.emplace_back(vocabulary_match(word.text, query, word.highlights,
					                                    30 + std::min(word.occurrences, 20), icon_index::tag));
				}

				if (found.size() < max_predictions)
				{
					for (const auto& word : *known)
					{
						found.emplace_back(std::make_shared<text_match>(*this, std::string(word.first), query,
						                                                ui::match_highlights{}, word.second));
					}
				}

				if (found.size() < max_predictions)
				{
					const auto found_groups = _state.item_index.auto_complete_words("@", max_predictions);

					for (const auto& word : found_groups)
					{
						found.emplace_back(vocabulary_match(word.text, query, word.highlights,
						                                    calc_auto_complete_word_weight(word, *known),
						                                    icon_index::search));
					}
				}

				if (found.size() < max_predictions)
				{
					const auto found_tags = _state.item_index.auto_complete_words("#", max_predictions);

					for (const auto& word : found_tags)
					{
						found.emplace_back(vocabulary_match(word.text, query, word.highlights,
						                                    calc_auto_complete_word_weight(word, *known),
						                                    icon_index::tag));
					}
				}
			}

			if (found.size() < max_predictions)
			{
				for (const auto& k : *known)
				{
					if (core::starts(k.first, trimmed_query) && found.size() < max_predictions)
					{
						found.emplace_back(std::make_shared<text_match>(*this, std::string(k.first), std::string{}));
					}
				}
			}

			// Rank higher-weight suggestions first (literal query, contextual/scoped values, exact/recent
			// matches), then by how many items each finds, preserving discovery order for equal weights.
			// Recent searches keep their order: how recent a search is, is why it is offered.
			const auto showing_recents = query_parts.empty() || core::icmp(query, current_search_text) == 0;

			if (!count_predictions(found, search_generation, !showing_recents, count_token, counter))
			{
				return;
			}

			if (found.size() > max_predictions) found.resize(max_predictions);
			const auto overview = _state.item_index.faces().current_overview();
			auto jobs = portrait_jobs(overview, found);
			_state.queue_ui([keep_alive, complete, found, search_generation, overview, jobs = std::move(jobs)]() mutable
			{
				if (keep_alive->_search_generation != search_generation) return;
				complete(found);
				if (keep_alive->_search_generation == search_generation)
					keep_alive->load_portraits(overview, std::move(jobs), search_generation);
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

	ui::auto_complete_match_ptr selected() const override { return _selected; }
};

std::shared_ptr<ui::complete_strategy_t> make_search_auto_complete(view_state& state,
                                                                   std::function<void(std::string)> set_text,
                                                                   std::function<void()> repaint)
{
	return std::make_shared<search_auto_complete>(state, std::move(set_text), std::move(repaint));
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
			style.colors = {ui::style::colors.toolbar_background, ui::style::colors.view_text,
			                ui::style::colors.view_selected_background};

			_search_predictions_frame = std::make_shared<ui::list_frame>();
			_search_predictions_frame->init(_app_frame, _search_completes, style);
		}

		if (_search_has_focus)
		{
			_search_session.begin(_search_edit->window_text());
			_search_completes->initialise([&p = _search_predictions_frame](const ui::auto_complete_results& results)
			{ p->show_results(results); });
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
		const auto& results = _search_predictions_frame->results();
		_search_predictions_frame->selected(results.empty() ? nullptr : results.front(),
		                                    ui::complete_strategy_t::select_type::init);
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
	_search_edit->window_text(text);
}

void app_frame::preview_search_prediction(const std::string& text)
{
	_search_session.preview(text);
	set_search_edit_text(text);
}

void app_frame::init_search()
{
	_search_completes = make_search_auto_complete(_state,
		[this](const std::string& text) { preview_search_prediction(text); },
		[this] { if (_search_predictions_frame) _search_predictions_frame->frame()->invalidate(); });
}
