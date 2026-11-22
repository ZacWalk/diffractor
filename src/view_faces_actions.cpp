// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: People review commands, the popup that names faces, undoable face placements, saving names to files, CSV,
// and scan controls.
// Owned by docs/faces.md#9-the-faces-view.

#include "pch.h"
#include "view_faces.h"

#include "app_command_status.h"
#include "app_commands.h"
#include "app_match.h"
#include "app_text.h"
#include "app_util.h"
#include "files.h"
#include "model_index.h"
#include "ui_dialog.h"
#include "view_list.h"

namespace
{
	std::string count_faces(const size_t n)
	{
		return format_plural_text(tt.faces_count_fmt, static_cast<int64_t>(n));
	}

	// About how long a scan of `pictures` takes, at the rate measured so far or, before there is one, at what this
	// machine's face threads read.
	// How long reading this many photos and videos takes: at the rate the scan has measured, or before there is one, at
	// the pace a face worker reads a photo and a video at.
	std::string about_how_long(const uint32_t photos, const uint32_t videos, const double rate)
	{
		constexpr auto photos_a_second = 5.0;
		constexpr auto seconds_a_video = 4.0;
		const auto workers = static_cast<double>(std::max(1, index_state::face_threads()));
		// Any rate measured is taken over the guess: a scan of long videos can read fewer than one a few seconds.
		const auto seconds = rate > 0.0 ? (photos + videos) / rate
		                                : (photos / photos_a_second + videos * seconds_a_video) / workers;
		const auto minutes = static_cast<int64_t>(std::ceil(seconds / 60.0));
		if (minutes < 60) return format_plural_text(tt.faces_minutes_fmt, std::max<int64_t>(1, minutes));
		return format_plural_text(tt.faces_hours_fmt, (minutes + 30) / 60);
	}

	// What face search reads, counted: pictures, videos, or both.
	std::string count_read(const faces::face_search& faces, const uint32_t n)
	{
		const auto& format = !faces.reads_videos()   ? tt.faces_pictures_fmt
		                     : !faces.reads_photos() ? tt.faces_videos_fmt
		                                             : tt.faces_items_fmt;
		return format_plural_text(format, static_cast<int64_t>(n));
	}

	bool ask_to_confirm(const ui::frame_ptr& parent, const icon_index icon, const std::string_view title,
	                    const std::string_view text, const std::string_view ok)
	{
		const auto dlg = make_dlg(parent);
		const std::vector<view_element_ptr> controls = {
			set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon, title, text)),
			std::make_shared<divider_element>(),
			std::make_shared<ui::ok_cancel_control>(dlg->_frame, ok),
		};
		return dlg->show_modal(controls) == ui::close_result::ok;
	}

	// A face book is a document, which a refusal guards from a gigabyte of something that is not one.
	constexpr uint64_t max_face_book_bytes = 512ull * 1024 * 1024;

	// A name typed for a person, completed from the names the collection and the face book hold.
	bool ask_for_name(const ui::frame_ptr& parent, const std::string_view title, const std::string_view info,
	                  std::string& name, const std::vector<std::string>& names, const std::string_view field,
	                  const std::string_view accept)
	{
		const auto dlg = make_dlg(parent);
		const std::vector<view_element_ptr> controls = {
			set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon_index::person, title, info)),
			std::make_shared<divider_element>(),
			set_margin(std::make_shared<ui::edit_control>(dlg->_frame, field, name, names)),
			std::make_shared<divider_element>(),
			std::make_shared<ui::ok_cancel_control>(dlg->_frame, accept),
		};

		if (dlg->show_modal(controls) != ui::close_result::ok) return false;
		name = std::string(core::trim(name));
		return true;
	}

	// docs/faces.md#95-naming-a-face: the names given faces this session, the latest first, kept nowhere else. UI
	// thread only.
	std::vector<std::string>& names_given()
	{
		static std::vector<std::string> names;
		return names;
	}

	void remember_name_given(const std::string_view name)
	{
		constexpr size_t kept = 12;
		auto& names = names_given();
		std::erase_if(names, [name](const std::string& n) { return core::icmp(n, name) == 0; });
		names.insert(names.begin(), std::string(name));
		if (names.size() > kept) names.resize(kept);
	}

	// Where a face is: its picture, its box as fractions of the upright picture, and in a video the frame it is in.
	struct face_spot
	{
		core::file_path path;
		rectd area;
		std::optional<uint32_t> frame_ms;
	};

	face_spot spot_of(const faces::face_store& store, const uint32_t face)
	{
		const auto& run = store.run_of_face(face);
		return {run.path, store.entry(face).bounds(),
		        run.video ? std::optional(store.entry(face).time_ms) : std::nullopt};
	}

	// Face tiles cut already, so a picker opened again shows its faces at once. UI thread only.
	core::hash_map<std::string, core::const_surface_ptr>& face_tiles()
	{
		static core::hash_map<std::string, core::const_surface_ptr> tiles;
		return tiles;
	}

	std::string face_tile_key(const face_spot& s, const int edge)
	{
		return std::format("{}|{}|{}|{}|{}|{}|{}", s.path.str(), s.area.X, s.area.Y, s.area.Width, s.area.Height,
		                   s.frame_ms.value_or(UINT32_MAX), edge);
	}

	// The faces' tiles, `edge` pixels square, handed to `done` on the UI thread in the faces' order, empty where a face
	// cannot be cut. Those not cut already are cut on a worker, each picture decoded as large as its face needs, as
	// the sidebar cuts its portraits.
	void cut_face_tiles(view_state& state, std::vector<face_spot> spots, const int edge,
	                    std::function<void(std::vector<core::const_surface_ptr>)> done)
	{
		std::vector<core::const_surface_ptr> tiles(spots.size());
		auto missing = false;
		for (size_t i = 0; i < spots.size(); ++i)
		{
			if (const auto found = face_tiles().find(face_tile_key(spots[i], edge)); found != face_tiles().end())
				tiles[i] = found->second;
			else missing = true;
		}
		if (!missing)
		{
			done(std::move(tiles));
			return;
		}

		state.queue_async(async_queue::thumbnail_decode, [&state, spots = std::move(spots), edge,
		                                                  tiles = std::move(tiles), done = std::move(done)]() mutable
		{
			files ff;
			for (size_t i = 0; i < spots.size() && !platform::is_closing; ++i)
			{
				const auto& s = spots[i];
				if (tiles[i] || s.area.Width <= 0.0) continue;
				const auto item = state.item_index.find_item(s.path);
				const auto from_video = s.frame_ms.has_value();
				if (!item.ft || (item.ft->group != &file_group::photo && !from_video)) continue;

				try
				{
					const auto scale = std::max(s.area.Width, s.area.Height) * 1.6;
					const auto bound = static_cast<int>(std::clamp(std::ceil(edge / scale), static_cast<double>(edge),
					                                               static_cast<double>(faces::defaults().decode_edge)));
					tiles[i] = faces::cut_tile(faces::decode_face_source(ff, s.path, item.ft, from_video,
					                                                     s.frame_ms.value_or(0), {bound, bound}),
					                           s.area, edge);
				}
				catch (const std::exception& e)
				{
					core::log(__FUNCTION__, e.what());
				}
			}

			state.queue_ui([spots = std::move(spots), edge, tiles = std::move(tiles), done = std::move(done)]() mutable
			{
				constexpr size_t kept = 96;
				auto& cache = face_tiles();
				for (size_t i = 0; i < spots.size(); ++i)
				{
					if (!tiles[i]) continue;
					if (cache.size() >= kept) cache.erase(cache.begin());
					cache[face_tile_key(spots[i], edge)] = tiles[i];
				}
				done(std::move(tiles));
			});
		});
	}

	// docs/faces.md#95-naming-a-face: whom faces are, picked as Copy to folder picks a folder. Empty, it lists the people
	// the faces most likely are, then the names given this session, then the people in the most pictures; typed, the
	// names that match in that order, and a new person where nobody is called what was typed. A double-click answers.
	class person_picker final : public ui::complete_strategy_t, public std::enable_shared_from_this<person_picker>
	{
		view_state& _state;
		ui::frame_ptr _dialog;
		std::weak_ptr<ui::search_control> _search;
		std::vector<std::string> _likely;
		std::vector<std::string> _recent;
		std::vector<std::string> _leave_out;
		bool _preselect = true;
		person_vocabulary _people;
		std::function<bool(std::string_view)> _known;
		faces::overview_ptr _overview;
		ui::auto_complete_match_ptr _selected;
		uint64_t _generation = 0;

		bool left_out(const std::string_view name) const
		{
			return std::ranges::any_of(_leave_out, [name](const std::string& n) { return core::icmp(n, name) == 0; });
		}

		// Where a name comes in the list: the people the faces most likely are, then the names given this session.
		size_t rank(const std::string_view name) const
		{
			const auto in = [name](const std::vector<std::string>& names)
			{
				return static_cast<size_t>(std::ranges::find_if(names, [name](const std::string& n) {
					return core::icmp(n, name) == 0;
				}) - names.begin());
			};
			if (const auto at = in(_likely); at < _likely.size()) return at;
			if (const auto at = in(_recent); at < _recent.size()) return _likely.size() + at;
			return _likely.size() + _recent.size();
		}

		void load_portraits(const ui::auto_complete_results& rows)
		{
			if (!_overview || !_overview->store) return;
			std::vector<face_spot> spots;
			std::vector<std::weak_ptr<text_match>> waiting;
			for (const auto& r : rows)
			{
				const auto row = std::dynamic_pointer_cast<text_match>(r);
				if (!row || row->person_name.empty() || row->portrait) continue;
				const auto* const person = _overview->find_person(row->person_name);
				if (!person || person->samples.empty()) continue;
				const auto face = person->samples.front();
				spots.push_back(spot_of(*_overview->store, face));
				waiting.push_back(row);
			}
			if (spots.empty()) return;

			constexpr auto portrait_edge = 64;
			cut_face_tiles(_state, std::move(spots), portrait_edge,
			               [weak = weak_from_this(), waiting = std::move(waiting),
			                generation = _generation](const std::vector<core::const_surface_ptr>& tiles)
			{
				const auto self = weak.lock();
				if (!self || self->_generation != generation) return;
				for (size_t i = 0; i < waiting.size() && i < tiles.size(); ++i)
					if (const auto row = waiting[i].lock(); row && tiles[i]) row->portrait = tiles[i];
				if (self->_dialog) self->_dialog->invalidate();
			});
		}

	public:
		person_picker(view_state& state, ui::frame_ptr dialog, std::vector<std::string> likely,
		              std::vector<std::string> recent, std::vector<std::string> leave_out, const bool preselect,
		              person_vocabulary people, std::function<bool(std::string_view)> known,
		              faces::overview_ptr overview) :
			_state(state), _dialog(std::move(dialog)), _likely(std::move(likely)), _recent(std::move(recent)),
			_leave_out(std::move(leave_out)), _preselect(preselect), _people(std::move(people)),
			_known(std::move(known)), _overview(std::move(overview))
		{
			resize_to_show_results = false;
			max_predictions = 9u;
			rows_shown = 9u;
		}

		void bind(const std::shared_ptr<ui::search_control>& search) { _search = search; }

		std::string no_results_message() override { return std::string(tt.faces_name_face_hint); }

		void initialise(std::function<void(const ui::auto_complete_results&)> complete) override
		{
			search({}, std::move(complete));
		}

		// The rows for what is typed: for the list, and for tests.
		ui::auto_complete_results rows_for(const std::string_view query)
		{
			const auto typed = core::trim(query);
			auto words = _people(typed, 500);
			const auto someone = std::ranges::any_of(words, [typed](const person_word& w) {
				return core::icmp(w.text, typed) == 0;
			}) || (_known && _known(typed));
			std::erase_if(words, [this](const person_word& w) { return left_out(w.text); });
			std::ranges::stable_sort(words, [this](const person_word& a, const person_word& b)
			{ return rank(a.text) < rank(b.text); });

			ui::auto_complete_results rows;
			for (const auto& w : words)
			{
				if (rows.size() == max_predictions) break;
				auto row = std::make_shared<text_match>(*this, w.text, std::string{}, w.highlights, 1,
				                                        icon_index::person);
				row->person_name = w.text;
				if (w.occurrences > 0) row->count = static_cast<uint64_t>(w.occurrences);
				rows.push_back(std::move(row));
			}

			// A name nobody is called yet, which naming the faces makes a person of: last, so a name being typed
			// still finds who it starts.
			if (!typed.empty() && !someone)
			{
				auto row = std::make_shared<text_match>(*this, std::string(typed), std::string{},
				                                        ui::match_highlights{}, 0, icon_index::add);
				row->label = str_format(tt.faces_new_person_fmt, typed);
				if (rows.size() == max_predictions) rows.back() = std::move(row);
				else rows.push_back(std::move(row));
			}
			return rows;
		}

		void search(const std::string& query,
		            const std::function<void(const ui::auto_complete_results&)> complete) override
		{
			++_generation;
			auto rows = rows_for(query);
			// Typed, the first that matches is chosen, as a picker chooses; empty, only the person the faces most
			// likely are, and only where naming them as that person is not a rename.
			const auto typed = !core::trim(query).empty();
			const auto likely_first = !rows.empty() && !_likely.empty() &&
			                          core::icmp(rows.front()->edit_text(), _likely.front()) == 0;
			auto_select_first = !rows.empty() && (typed || (_preselect && likely_first));
			complete(rows);
			load_portraits(rows);
		}

		void selected(const ui::auto_complete_match_ptr& i, const select_type st) override
		{
			_selected = i;
			if (!i) return;

			// A row chosen by the keys or the pointer becomes what the box says; one chosen as the list fills, does not,
			// so what is being typed stays as typed.
			if (st != select_type::init)
				if (const auto search = _search.lock()) search->update_edit_text(i->edit_text());

			if (st == select_type::double_click && _dialog) _dialog->close(false);
		}

		ui::auto_complete_match_ptr selected() const override { return _selected; }
	};

	// What naming faces asks: the title, what it says beneath it and the faces it shows, the people offered first,
	// whom never to offer, whether the most likely starts chosen, and whether to offer going on to the next picture.
	struct face_naming
	{
		std::string title;
		std::string details;
		std::vector<face_spot> faces;
		size_t count = 0;
		std::vector<std::string> likely;
		std::vector<std::string> leave_out;
		bool preselect = true;
		bool offer_next = false;
	};

	struct face_name_answer
	{
		std::string name;
		bool next = false;
	};

	std::vector<std::string> names_of(const std::vector<faces::likely_person>& likely)
	{
		std::vector<std::string> names;
		for (const auto& p : likely) names.push_back(p.name);
		return names;
	}

	// docs/faces.md#95-naming-a-face: the popup that asks whom faces are, laid out as Copy to folder is.
	std::optional<face_name_answer> ask_face_name(view_state& state, const ui::frame_ptr& parent, const face_naming& n)
	{
		const auto dlg = make_dlg(parent);
		const auto frame = dlg->_frame;
		auto& faces = state.item_index.faces();
		std::string text;
		const auto picker = std::make_shared<person_picker>(state, frame, n.likely, names_given(), n.leave_out,
		                                                    n.preselect,
		                                                    [&index = state.item_index](const std::string_view query,
		                                                                                const size_t limit)
		{
			std::vector<person_word> words;
			for (auto& w : index.auto_complete_people(query, limit))
				words.push_back({std::move(w.text), std::move(w.highlights), w.occurrences});
			return words;
		},
		                                                    [&faces](const std::string_view name)
		{ return !name.empty() && faces.book().people()->named(name) != nullptr; }, faces.current_overview());
		const auto search = std::make_shared<ui::search_control>(frame, text, picker);
		picker->bind(search);

		const auto title = std::make_shared<ui::title_control2>(frame, icon_index::person, n.title, n.details,
		                                                        std::vector<core::const_surface_ptr>{}, n.count);
		if (!n.faces.empty())
		{
			constexpr auto title_edge = 112;
			cut_face_tiles(state, n.faces, title_edge,
			               [weak = std::weak_ptr(title), frame, details = n.details,
			                count = n.count](std::vector<core::const_surface_ptr> tiles)
			{
				const auto shown = weak.lock();
				if (!shown) return;
				std::erase(tiles, nullptr);
				shown->selection(details, tiles, count);
				frame->layout();
				frame->invalidate();
			});
		}

		std::vector<view_element_ptr> controls = {set_margin(title), std::make_shared<divider_element>(), search};
		if (n.offer_next)
			controls.push_back(
				std::make_shared<ui::check_control>(frame, tt.faces_name_next, setting.faces.next_after_naming));
		controls.push_back(std::make_shared<divider_element>());
		controls.push_back(std::make_shared<ui::ok_cancel_control>(frame, n.title));

		if (dlg->show_modal(controls, {52}) != ui::close_result::ok) return std::nullopt;
		auto name = picker->selected() ? picker->selected()->edit_text() : std::string(core::trim(text));
		if (name.empty()) return std::nullopt;
		remember_name_given(name);
		return face_name_answer{std::move(name), n.offer_next && setting.faces.next_after_naming};
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// What is selected
///////////////////////////////////////////////////////////////////////////////////////////////////

bool faces_view::is_row_selected(const int row) const
{
	return row >= 0 && row < static_cast<int>(_rows.size()) && _people_selection.is_selected(_rows[row].key);
}

std::vector<int> faces_view::selected_rows() const
{
	std::vector<int> result;
	if (_people_selection.empty()) return result;

	for (size_t i = 0; i < _rows.size(); ++i)
		if (_people_selection.is_selected(_rows[i].key)) result.push_back(static_cast<int>(i));
	return result;
}

int faces_view::focus_row() const
{
	const auto& cursor = _people_selection.cursor();
	return cursor ? find_row(*cursor) : -1;
}

std::vector<std::string> faces_view::selectable_row_keys() const
{
	std::vector<std::string> keys;
	for (const auto& r : _rows)
		if (r.what != row::kind::surname && !r.bounds.is_empty()) keys.push_back(r.key);
	return keys;
}

std::vector<int> faces_view::selected_cells() const
{
	std::vector<int> result;
	for (size_t i = 0; i < _cell_keys.size(); ++i)
		if (_face_selection.is_selected(_cell_keys[i])) result.push_back(static_cast<int>(i));
	return result;
}

bool faces_view::is_cell_selected(const int cell) const
{
	return cell >= 0 && cell < static_cast<int>(_cell_keys.size()) && _face_selection.is_selected(_cell_keys[cell]);
}

int faces_view::focus_cell() const
{
	const auto& cursor = _face_selection.cursor();
	if (!cursor) return -1;
	const auto found = std::ranges::find(_cell_keys, *cursor);
	return found == _cell_keys.end() ? -1 : static_cast<int>(found - _cell_keys.begin());
}

void faces_view::show_selected_rows()
{
	_cells.clear();
	_cell_keys.clear();
	_headings.clear();
	_face_selection.clear();
	_pending_faces_anchor.reset();
	_unknown_order = {};
	_reveal_face = false;
	_faces_scroller.reset();
	build_cells();
}

void faces_view::select_rows(const std::vector<int>& rows)
{
	std::vector<std::string> keys;
	for (const auto r : rows)
	{
		if (r >= 0 && r < static_cast<int>(_rows.size()) && _rows[r].what != row::kind::surname)
			keys.push_back(_rows[r].key);
	}

	_pane = pane::groups;
	_people_selection.clear();
	_people_selection.choose(keys, ui::list_selection<std::string>::choice::replace);
	show_selected_rows();
}

void faces_view::select_cells(const std::vector<int>& cells)
{

	std::vector<std::string> keys;
	for (const auto c : cells)
		if (c >= 0 && c < static_cast<int>(_cell_keys.size())) keys.push_back(_cell_keys[c]);
	_face_selection.clear();
	_face_selection.choose(keys, ui::list_selection<std::string>::choice::replace);
	_pane = pane::faces;
	changed(false);
}

void faces_view::select_all()
{
	if (_pane == pane::faces)
	{
		_face_selection.select_all(_cell_keys);
		changed(false);
		return;
	}

	std::vector<int> rows;
	for (size_t i = 0; i < _rows.size(); ++i)
		if (!_rows[i].bounds.is_empty() && _rows[i].what != row::kind::surname) rows.push_back(static_cast<int>(i));
	select_rows(rows);
}

bool faces_view::can_rename_row(const int row) const
{
	return !_combining && row >= 0 && row < static_cast<int>(_rows.size()) && _rows[row].named() && book_editable();
}

bool faces_view::book_editable() const
{
	return _state.item_index.faces().can_change_book();
}

std::vector<std::string> faces_view::known_names() const
{
	std::vector<std::string> result;
	core::hash_set<std::string, core::ihash, core::ieq> seen;
	const auto add = [&](const std::string& name)
	{
		if (!name.empty() && result.size() < 1000 && seen.insert(name).second) result.push_back(name);
	};

	for (const auto& p : _state.item_index.faces().book().people()->people) add(p.name);
	if (_model)
	{
		if (_model->overview)
			for (const auto& p : _model->overview->people) add(p.name);
		for (const auto& p : _model->people) add(p.name);
	}
	return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The face book
///////////////////////////////////////////////////////////////////////////////////////////////////

faces::face_ref faces_view::ref_of(const faces_view_model::shown& s) const
{
	faces::face_ref ref{s.path, s.area, 0};
	if (s.face && _model && _model->store && *s.face < _model->store->face_count())
	{
		const auto& e = _model->store->entry(*s.face);
		ref.box = e.bounds();
		ref.position = e.position;
		if (e.has_hash) ref.signature = _model->store->signature(*s.face);
	}
	return ref;
}

faces_view::moved_face faces_view::placement_of(const uint32_t face) const
{
	if (const auto it = _moved.find(face); it != _moved.end()) return it->second;
	moved_face where;
	where.row_key = "unknown";
	if (!_model || !_model->overview) return where;
	const auto& o = *_model->overview;
	if (const auto* person = o.person_of_face(face))
	{
		const auto& member = *o.member_of_face(face);
		const auto key = person->id ? std::format("p:{}", person->id) : "n:" + core::to_lower(person->name);
		if (member.kind == faces::member_kind::suggested)
		{
			where.row_key.clear();
			where.offer_key = key;
		}
		else where.row_key = key;
		where.kind = member.kind;
		where.score = member.score;
	}
	else if (std::ranges::binary_search(o.hidden, face)) where.row_key = "hidden";
	return where;
}

void faces_view::apply_placement(const std::vector<int>& cells, const faces::face_action action,
                                 const std::string_view name)
{
	using action_t = faces::face_action;
	const row_hold hold(*this);
	faces::placement_batch batch;
	batch.name = core::trim(name);
	auto& book = _state.item_index.faces().book();
	core::hash_map<uint32_t, moved_face> before;
	for (const auto i : cells)
	{
		if (i < 0 || i >= static_cast<int>(_cells.size())) continue;
		const auto& c = _cells[i];
		if (!c.shown.face || (action == action_t::hide && c.hidden) || (action == action_t::unhide && !c.hidden))
			continue;
		if (before.emplace(*c.shown.face, placement_of(*c.shown.face)).second) batch.faces.push_back(ref_of(c.shown));
	}
	if (batch.faces.empty()) return;
	auto edit = book.place(std::span(&batch, 1), action, _book_generation);
	if (!edit) return;
	_before_placement = std::move(before);
	const auto assigned = action == action_t::assign ? edit->people.front() : 0;
	const auto key = assigned ? std::format("p:{}", assigned) : action == action_t::hide ? "hidden" : "unknown";
	for (const auto& [face, was] : _before_placement)
	{
		moved_face now;
		now.generation = edit->generation;
		now.row_key = key;
		if (assigned)
		{
			now.kind = faces::member_kind::confirmed;
			now.score = was.offer_key == key || was.row_key == key ? was.score : faces::not_compared;
		}
		if (action == action_t::unhide)
		{
			const auto& e = _model->store->entry(face);
			const auto path = _model->store->run_of_face(face).path;
			const auto marks = book.marks(path);
			const auto exact = e.has_hash ? book.signature_mark(_model->store->signature(face)) : std::nullopt;
			if (const auto* m = exact ? &*exact : faces::find_mark(marks, e.bounds(), e.position); m && m->person)
			{
				now.kind = faces::member_kind::confirmed;
				now.row_key = std::format("p:{}", m->person);
			}
		}
		_moved[face] = std::move(now);
	}
	const auto destination = assigned                   ? edit->after->person(assigned)->name
	                         : action == action_t::hide ? std::string(tt.faces_hidden_section)
	                                                    : std::string(tt.unknown);
	const auto n = count_faces(batch.faces.size());
	const auto affected = batch.faces.size() == 1 ? batch.faces.front().path.name().sv() : std::string_view(n);
	if (action == action_t::assign || action == action_t::reject)
		_undo_detail = std::format("{}: {}",
		                           str_format(action == action_t::assign ? tt.faces_confirm_person_fmt
		                                                                 : tt.faces_not_person_fmt,
		                                      batch.name),
		                           affected);
	else
		_undo_detail = std::format("{}: {}",
		                           action == action_t::hide ? tt.command_face_hide.sv()
		                                                    : tt.command_face_show_again.sv(),
		                           affected);
	if (action == action_t::unhide)
	{
		// Show preserves the old identity. A mixed selection need not have one destination.
		_destination = {};
		_last_placement = std::format("{}: {}", tt.command_face_show_again, n);
	}
	else
	{
		_destination = {destination, key, assigned, edit->generation};
		_last_placement = str_format(tt.faces_last_placement_fmt, n, destination);
		if (action == faces::face_action::reject)
			_last_placement += " " + str_format(tt.faces_not_person_fmt, batch.name);
	}
	_placement_edit = std::move(edit);
	build_rows();
}

std::string faces_view::placement_destination() const
{
	if (_destination.key.empty()) return {};
	const auto roster = _state.item_index.faces().book().people();
	if (roster->loaded_at > _destination.generation || (_destination.person && !roster->person(_destination.person)))
		return {};
	return _destination.label;
}

void faces_view::follow_placement()
{
	if (_holding_rows || _drag.active || placement_destination().empty()) return;
	_filter.clear();
	build_rows();
	const auto i = find_row(_destination.key);
	if (i < 0) return;
	_collapsed_surnames.erase(_rows[i].surname);
	layout_rows(nullptr);
	select_rows({i});
	ensure_row_visible(i);
}

bool faces_view::can_undo_placement() const
{
	return _placement_edit && _state.item_index.faces().book().can_undo(*_placement_edit);
}

void faces_view::undo_placement()
{
	auto& book = _state.item_index.faces().book();
	const row_hold hold(*this);
	const auto edit = std::exchange(_placement_edit, {});
	if (edit && book.undo(*edit))
	{
		if (!edit->reverse.people.empty()) request_model(); // A combination also changed how file names are displayed.
		for (auto& [face, m] : _before_placement)
		{
			m.generation = book.generation();
			_moved[face] = std::move(m);
		}
		_last_placement.clear();
		_destination = {};
	}
	_before_placement.clear();
	build_rows();
}

void faces_view::move_cells_to_name(const std::vector<int>& cells, const std::string_view name)
{
	if (!core::trim(name).empty()) apply_placement(cells, faces::face_action::assign, name);
}

void faces_view::move_cells_to_row(const std::vector<int>& cells, const int row)
{
	if (row < 0 || row >= static_cast<int>(_rows.size())) return;
	if (_rows[row].named()) move_cells_to_name(cells, _rows[row].title);
	else if (_rows[row].what == row::kind::hidden_faces) apply_placement(cells, faces::face_action::hide);
}

bool faces_view::can_drop_cell(const int i, const int row) const
{
	if (i < 0 || i >= static_cast<int>(_cells.size()) || !_cells[i].shown.face) return false;
	if (_rows[row].what == row::kind::hidden_faces) return !_cells[i].hidden;
	return _rows[row].named() &&
	       (_review_person != _rows[row].title || _cells[i].member == faces::member_kind::suggested);
}

bool faces_view::can_drop(const pane from, const std::vector<int>& items, const drop_target& target) const
{
	return from == pane::faces && book_editable() && target.row >= 0 && target.row < static_cast<int>(_rows.size()) &&
	       std::ranges::any_of(items, [&](int i) { return can_drop_cell(i, target.row); });
}

void faces_view::drop(const pane from, const std::vector<int>& items, const drop_target& target)
{
	if (!can_drop(from, items, target)) return;
	auto eligible = items;
	std::erase_if(eligible, [&](int i) { return !can_drop_cell(i, target.row); });
	move_cells_to_row(eligible, target.row);
}

faces_view::row_hold::row_hold(faces_view& view) :
	_view(view), _was_holding(view._holding_rows), _generation(view._book_generation)
{
	_view.hold_rows();
}

faces_view::row_hold::~row_hold()
{
	if (!_was_holding) _view.release_rows();
}

bool faces_view::row_hold::book_unchanged(const std::string_view command) const
{
	if (_view._state.item_index.faces().book().generation() == _generation) return true;
	core::log(__FUNCTION__, std::format("{} cancelled: the face book changed while it asked", command));
	return false;
}

bool faces_view::ask_name(const std::string_view title, const std::string_view info, std::string& name) const
{
	return ask_for_name(_host->owner(), title, info, name, known_names(), tt.face_name_label, tt.button_ok);
}

namespace
{
	// A face on a picture as the user saw it when they acted on it - its picture and box, whom it was shown as and
	// whether only as a suggestion - so a dialog left open across a change acts on nothing.
	struct picture_face
	{
		faces::face_ref ref;
		std::string name;
		bool offered = false;
		bool operator==(const picture_face& other) const
		{
			return ref.path == other.ref.path && ref.box == other.ref.box && name == other.name &&
			       offered == other.offered;
		}
	};

	// From the store and overview the picture's faces are shown from (docs/faces.md#93-faces-on-a-picture).
	std::optional<picture_face> find_picture_face(const faces::face_book& book, const uint32_t face)
	{
		const auto store = faces::active_store();
		if (!store || face >= store->face_count()) return {};
		const auto& entry = store->entry(face);
		picture_face found{{store->run_of_face(face).path, entry.bounds(), entry.position}};
		if (entry.has_hash) found.ref.signature = store->signature(face);
		const auto shown = faces::faces_on_picture(*store, faces::active_overview().get(), book, found.ref.path);
		const auto it = std::ranges::find(shown, face, &faces::face_on_picture::face);
		if (it == shown.end() || !(it->box == found.ref.box)) return {};
		found.name = it->name;
		found.offered = it->offered;
		return found;
	}
}

picture_named name_picture_face(view_state& state, const ui::frame_ptr& parent, const uint32_t face,
                                const picture_naming how, const bool offer_next)
{
	auto& faces = state.item_index.faces();
	auto& book = faces.book();
	if (!faces.can_change_book()) return {};
	const auto before = find_picture_face(book, face);
	if (!before) return {};

	// docs/faces.md#95-naming-a-face: a named face is renamed, so whom it is named as is not offered, and nobody starts
	// chosen; a suggestion is not its name until confirmed, and comes first unless it is named as someone else.
	const auto renaming = !before->offered && !before->name.empty();
	const auto current = renaming ? before->name : std::string{};
	std::vector<std::string> leave_out;
	if (!before->name.empty() && (renaming || how == picture_naming::someone_else)) leave_out.push_back(before->name);
	std::vector<std::string> likely;
	if (const auto o = faces::active_overview(); o && o->store == faces::active_store())
		likely = names_of(faces::likely_people(*o, std::span(&face, 1), leave_out));

	face_naming n;
	n.title = tt.faces_name_face;
	n.details = std::format("{}\n{}", before->ref.path.name(), tt.faces_name_face_hint);
	n.faces = {{before->ref.path, before->ref.box}};
	if (const auto store = faces::active_store(); store && face < store->face_count())
		n.faces.front().frame_ms = spot_of(*store, face).frame_ms;
	n.count = 1;
	n.likely = std::move(likely);
	n.leave_out = std::move(leave_out);
	n.preselect = !renaming;
	n.offer_next = offer_next;
	const auto answer = ask_face_name(state, parent, n);
	if (!answer || core::icmp(answer->name, current) == 0) return {false, answer && answer->next};
	if (find_picture_face(book, face) != before)
	{
		core::log(__FUNCTION__, "Naming cancelled because the face changed while the dialog was open.");
		return {};
	}

	const faces::placement_batch batch{0, answer->name, {before->ref}};
	return {book.place(std::span(&batch, 1), faces::face_action::assign, book.generation()).has_value(), answer->next};
}

picture_face_target only_face_on_picture(view_state& state, const core::file_path path)
{
	auto& faces = state.item_index.faces();
	if (!faces.is_enabled()) return {{}, tt.faces_name_picture_off.sv()};
	if (!faces.can_change_book()) return {{}, tt.faces_read_only.sv()};
	const auto store = faces::active_store();
	const auto shown = store && !path.is_empty()
	                       ? faces::faces_on_picture(*store, faces::active_overview().get(), faces.book(), path)
	                       : std::vector<faces::face_on_picture>{};
	if (shown.empty()) return {{}, tt.faces_name_picture_none.sv()};
	if (shown.size() > 1) return {{}, tt.faces_name_picture_several.sv()};
	return {shown.front().face, {}};
}

std::shared_ptr<ui::complete_strategy_t> make_person_picker_for_test(view_state& state, ui::frame_ptr dialog,
                                                                     std::vector<std::string> likely,
                                                                     std::vector<std::string> recent,
                                                                     std::vector<std::string> leave_out,
                                                                     const bool preselect, person_vocabulary people)
{
	return std::make_shared<person_picker>(state, std::move(dialog), std::move(likely), std::move(recent),
	                                       std::move(leave_out), preselect, std::move(people), nullptr, nullptr);
}

bool review_picture_face(view_state& state, const uint32_t face, const bool confirm)
{
	auto& faces = state.item_index.faces();
	auto& book = faces.book();
	if (!faces.can_change_book()) return false;
	const auto shown = find_picture_face(book, face);
	if (!shown || !shown->offered || shown->name.empty()) return false;
	const faces::placement_batch batch{0, shown->name, {shown->ref}};
	return book
	    .place(std::span(&batch, 1), confirm ? faces::face_action::assign : faces::face_action::reject,
	           book.generation())
	    .has_value();
}

bool faces_view::faces_targeted() const
{
	return _pane == pane::faces && !_face_selection.empty();
}

std::string_view faces_view::rename_text() const
{
	return tt.command_face_rename_person.sv();
}

std::string_view faces_view::confirm_text() const
{
	return _confirm_label.empty() ? tt.button_confirm.sv() : std::string_view(_confirm_label);
}

std::string_view faces_view::remove_text() const
{
	return _remove_label.empty() ? tt.faces_reject.sv() : std::string_view(_remove_label);
}

bool faces_view::can_rename() const
{
	const auto rows = selected_rows();
	return !faces_targeted() && rows.size() == 1 && can_rename_row(rows.front());
}

bool faces_view::can_hide() const
{
	return faces_targeted() && book_editable() &&
	       std::ranges::any_of(selected_cells(), [this](int c) { return _cells[c].shown.face && !_cells[c].hidden; });
}

bool faces_view::can_show_again() const
{
	return faces_targeted() && book_editable() &&
	       std::ranges::any_of(selected_cells(), [this](int c) { return _cells[c].shown.face && _cells[c].hidden; });
}

bool faces_view::can_name_faces() const
{
	return faces_targeted() && book_editable() &&
	       std::ranges::any_of(selected_cells(), [this](int c) { return _cells[c].shown.face.has_value(); });
}

bool faces_view::can_reject() const
{
	return !_review_person.empty() && can_name_faces();
}

bool faces_view::can_confirm() const
{
	return faces_targeted() && book_editable() && !_review_person.empty() &&
	       std::ranges::any_of(selected_cells(),
	                           [this](int c) { return _cells[c].member == faces::member_kind::suggested; });
}

const faces_view_model::shown* faces_view::single_selected_face() const
{
	if (!faces_targeted() || _face_selection.size() != 1) return nullptr;
	const auto cells = selected_cells();
	return cells.size() == 1 ? &_cells[cells.front()].shown : nullptr;
}

const faces_view_model::shown* faces_view::face_search_target() const
{
	const auto* const selected = single_selected_face();
	if (!selected || !selected->face || !_model || !_model->store) return nullptr;
	const auto face = *selected->face;
	return face < _model->store->face_count() && _model->store->entry(face).has_hash ? selected : nullptr;
}

std::string faces_view::person_search_target() const
{
	if (faces_targeted()) return {};

	const auto rows = selected_rows();
	if (rows.size() != 1) return {};
	const auto row = rows.front();
	const auto& r = _rows[row];
	return r.named() ? r.title : std::string{};
}

bool faces_view::can_search_face() const
{
	return face_search_target() != nullptr;
}

bool faces_view::can_search_person() const
{
	return !person_search_target().empty();
}

bool faces_view::can_open_in_folder() const
{
	const auto* const selected = single_selected_face();
	return selected && !selected->path.is_empty();
}

bool faces_view::can_save_names() const
{
	return book_editable() && _state.item_index.faces().book().mark_count() > 0;
}

void faces_view::rename()
{
	if (!can_rename()) return;
	const row_hold hold(*this);
	const auto rows = selected_rows();
	if (rows.size() != 1 || !can_rename_row(rows.front())) return;
	const auto row = rows.front();

	if (_rows[row].what == row::kind::file_person)
	{
		rename_file_person(row);
		return;
	}

	const auto from = _rows[row].title;
	auto to = from;
	if (ask_name(tt.command_face_rename_person, str_format(tt.face_rename_fmt, from), to) && !to.empty() &&
	    to != from && hold.book_unchanged("Rename"))
		rename_to(row, to);
}

// A collision consumes the rename, including on Cancel: neither the normal rename nor
// the file-metadata writer may run after this decision.
bool faces_view::offer_combine(const std::string_view from, const std::string_view to)
{
	auto& book = _state.item_index.faces().book();
	const auto roster = book.people();
	if (core::icmp(from, to) == 0) return false;
	std::string target;
	if (const auto* p = roster->named(to)) target = p->name;
	else if (const auto* file_person = _model ? _model->find_person(to) : nullptr) target = file_person->name;
	else if (const auto* reviewed = _model && _model->overview ? _model->overview->find_person(to) : nullptr)
		target = reviewed->name;
	else return false;
	if (core::icmp(from, target) == 0) return true; // Already another spelling of this person.

	const auto generation = _book_generation;
	const std::string source(from);
	if (!_host || !ask_to_confirm(_host->owner(), icon_index::person, tt.faces_combine_title,
	                              str_format(tt.faces_combine_fmt, source, target), tt.button_ok))
		return true;
	_combining = true;
	_state.queue_async(async_queue::work,
	                   [weak = weak_from_this(), &book, &async = _state._async, generation, source, target]
	{
		auto edit = book.combine(source, target, generation);
		async.queue_ui([weak, source, target, edit = std::move(edit)]() mutable
		{
			const auto self = weak.lock();
			if (!self) return;
			self->_combining = false;
			if (!edit)
			{
				core::log(__FUNCTION__, "Combine cancelled: person identities changed");
				if (self->_active && self->_host)
					make_dlg(self->_host->owner())
						->show_message(icon_index::person, tt.faces_combine_title, tt.faces_combine_changed);
				return;
			}
			if (!self->_active || !self->_state.item_index.faces().book().can_undo(*edit)) return;
			self->_before_placement.clear();
			self->_moved.clear();
			const auto id = edit->after->named(target)->id;
			self->_destination = {target, std::format("p:{}", id), id, edit->generation};
			self->_last_placement = str_format(tt.faces_combined_fmt, source, target);
			self->_undo_detail = self->_last_placement;
			self->_placement_edit = std::move(edit);
			self->_filter.clear();
			self->_open_person = target;
			self->build_rows();
			self->request_model();
		});
	});
	return true;
}

void faces_view::rename_to(const int i, const std::string_view name)
{
	if (!can_rename_row(i) || !_rows[i].book_person || name.empty()) return;
	auto& book = _state.item_index.faces().book();
	const auto from = _rows[i].title;
	if (offer_combine(from, name)) return;

	// docs/faces.md#61-named-people: pictures that name the person in their own metadata keep that name until it is
	// changed there too. Asked whether to, the old name stays a former name either way, so the person stays one row.
	std::vector<core::file_path> paths;
	std::string file_name;
	if (const auto* const file_person = _model ? _model->find_person(from) : nullptr)
	{
		file_name = file_person->name;
		core::hash_set<core::file_path, core::ihash, core::ieq> seen;
		for (const auto& named : file_person->named)
			if (seen.insert(named.path).second) paths.push_back(named.path);
	}

	enum class in_files
	{
		no,
		yes,
		cancel,
	};
	auto choice = in_files::no;
	if (!paths.empty() && _host)
	{
		choice = in_files::cancel;
		const auto dlg = make_dlg(_host->owner());
		const auto frame = dlg->_frame;
		const auto text = str_format(tt.face_rename_in_files_fmt, from,
		                             format_plural_text(tt.faces_pictures_fmt, static_cast<int64_t>(paths.size())),
		                             name);
		const std::vector<view_element_ptr> controls = {
			set_margin(
				std::make_shared<ui::title_control2>(frame, icon_index::rename, tt.command_face_rename_person, text)),
			std::make_shared<divider_element>(),
			std::make_shared<ui::button_row>(std::vector<ui::control_ptr>{ui::make_button(tt.face_rename_files,
		                                                                                  [frame, &choice]
		{
			choice = in_files::yes;
			frame->close(false);
		}, true),
		                                                                  ui::make_button(tt.face_rename_book_only,
		                                                                                  [frame, &choice]
		{
			choice = in_files::no;
			frame->close(false);
		}),
		                                                                  ui::make_button(tt.button_cancel, [frame]
		{ frame->close(true); })},
		                                     ui::button_row_fit::wrap),
		};
		if (dlg->show_modal(controls) != ui::close_result::ok) choice = in_files::cancel;
	}
	if (choice == in_files::cancel) return;

	if (!book.rename(_rows[i].book_person, name, !paths.empty()))
	{
		if (_host)
			make_dlg(_host->owner())
				->show_message(icon_index::person, tt.command_face_rename_person, tt.faces_join_name_taken);
		return;
	}

	if (choice == in_files::yes)
	{
		face_edits_by_path by_path;
		for (const auto& path : paths)
			by_path[path].edits.push_back({face_name_edit::kind::rename, {}, file_name, std::string(name)});
		_open_person = std::string(name);
		write_face_edits(std::move(by_path), tt.command_face_rename_person,
		                 std::format("{}: {} \u2192 {}", tt.command_face_rename_person, from, name));
	}
	build_rows();
}

void faces_view::name_faces()
{
	if (can_name_faces()) name_cells(selected_cells(), false);
}

void faces_view::name_cells(const std::vector<int>& cells, const bool one_face)
{
	// The name is asked for in the window; a view without one asks nobody.
	if (!_host || !book_editable()) return;
	const row_hold hold(*this);

	// docs/faces.md#95-naming-a-face: whom the faces are already shown as is never offered; and faces every one of
	// which is named already are being renamed, so nobody starts chosen.
	const auto o = _model ? _model->overview : nullptr;
	std::vector<uint32_t> faces;
	std::vector<std::string> shown_as;
	std::vector<face_spot> spots;
	auto renaming = true;
	if (!_review_person.empty()) shown_as.push_back(_review_person);
	for (const auto c : cells)
	{
		if (c < 0 || c >= static_cast<int>(_cells.size()) || !_cells[c].shown.face) continue;
		const auto& cell = _cells[c];
		faces.push_back(*cell.shown.face);
		if (const auto* const p = o ? o->person_of_face(faces.back()) : nullptr) shown_as.push_back(p->name);
		if (cell.shown.has_area() && spots.size() < 7)
			spots.push_back({cell.shown.path, cell.shown.area, cell.shown.frame_ms});
		renaming = renaming && faces::is_held(cell.member);
	}
	if (faces.empty()) return;

	face_naming n;
	n.title = one_face ? tt.faces_name_face : tt.faces_name_faces;
	n.details = std::format("{}\n{}",
	                        one_face ? std::string(_cells[cells.front()].shown.path.name())
	                                 : format_plural_text(tt.faces_selected_fmt, static_cast<int64_t>(faces.size())),
	                        tt.faces_name_face_hint);
	n.faces = std::move(spots);
	n.count = faces.size();
	n.likely = o ? names_of(faces::likely_people(*o, faces, shown_as)) : std::vector<std::string>{};
	n.leave_out = std::move(shown_as);
	n.preselect = !renaming;
	if (const auto answer = ask_face_name(_state, _host->owner(), n); answer && hold.book_unchanged("Name faces"))
		move_cells_to_name(cells, answer->name);
}

void faces_view::hide(const bool hidden)
{
	if (!faces_targeted()) return;
	apply_placement(selected_cells(), hidden ? faces::face_action::hide : faces::face_action::unhide);
}

void faces_view::confirm()
{
	if (!can_confirm()) return;
	auto cells = selected_cells();
	std::erase_if(cells, [this](int i) { return _cells[i].member != faces::member_kind::suggested; });
	apply_placement(cells, faces::face_action::assign, _review_person);
}

void faces_view::reject()
{
	if (can_reject()) apply_placement(selected_cells(), faces::face_action::reject, _review_person);
}

std::string_view faces_view::heading_action_text(const heading_action action) const
{
	switch (action)
	{
	case heading_action::name: return tt.faces_name_all.sv();
	case heading_action::hide: return tt.faces_hide_all.sv();
	case heading_action::confirm_all: break;
	}
	return tt.faces_confirm_all.sv();
}

icon_index faces_view::heading_action_icon(const heading_action action)
{
	switch (action)
	{
	case heading_action::name: return icon_index::person;
	case heading_action::hide: return icon_index::cancel;
	case heading_action::confirm_all: break;
	}
	return icon_index::check;
}

void faces_view::place_all_under(const int heading)
{
	if (heading < 0 || heading >= static_cast<int>(_headings.size()) ||
	    !_headings[heading].has(heading_action::confirm_all))
		return;
	apply_placement(cells_under(heading), faces::face_action::assign, _review_person);
}

void faces_view::ask_to_place_all(const int heading)
{
	const row_hold hold(*this);
	if (heading < 0 || heading >= static_cast<int>(_headings.size()) ||
	    !_headings[heading].has(heading_action::confirm_all) || !book_editable())
		return;
	if (ask_to_confirm(_host->owner(), icon_index::person, str_format(tt.faces_confirm_person_fmt, _review_person),
	                   count_faces(cells_under(heading).size()), tt.faces_confirm_all) &&
	    hold.book_unchanged("Confirm all"))
		place_all_under(heading);
}

void faces_view::press_heading_button(const int heading, const heading_action action)
{
	if (heading < 0 || heading >= static_cast<int>(_headings.size()) || !_headings[heading].has(action) ||
	    !book_editable())
		return;

	if (action == heading_action::confirm_all)
	{
		ask_to_place_all(heading);
		return;
	}

	// The faces under the heading are selected, as a click on it selects them, so naming them asks as F2 does and
	// hiding them leaves the receipt H leaves, with its Undo.
	select_cells(cells_under(heading));
	if (action == heading_action::name) name_faces();
	else hide(true);
}

void faces_view::open_items(const df::search_t& search, const core::unique_paths& selection)
{
	if (!_state.open(_host, search, selection)) return;

	// An explicit open is a step forward even if Items already has that search behind Faces.
	if (_state.history.current_view() == view_type::faces)
		_state.history.history_add(search, _state.selected_items().ids());
	_state.view_mode(view_type::items);
}

void faces_view::search_face()
{
	if (const auto* const target = face_search_target())
	{
		const auto position = _model->store->entry(*target->face).position + 1;
		open_items(df::search_for_face(target->path.name(), position));
	}
}

void faces_view::search_person()
{
	if (const auto name = person_search_target(); !name.empty()) open_items(df::search_t().with(prop::person, name));
}

void faces_view::open_in_folder()
{
	if (const auto* const selected = single_selected_face(); selected && !selected->path.is_empty())
		open_picture_in_folder(selected->path);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Names in files: docs/faces.md#10-saving-names-to-files
///////////////////////////////////////////////////////////////////////////////////////////////////

face_name_plan plan_name_writes(const index_state& index, const faces::face_book& book)
{
	face_name_plan plan;
	const auto roster = book.people();

	auto by_path = book.all_marks();
	// A portable reference has no writable file. Only a current, exact local detection
	// supplies the picture and region on which the explicit Save names command can act.
	for (auto& [path, marks] : by_path) std::erase_if(marks, [](const auto& m) { return m.signature.has_value(); });
	const auto signatures = book.all_signatures();
	if (const auto store = index.faces().store(); store && !signatures.empty())
	{
		const auto runs = store->run_count();
		for (uint32_t r = 0; r < runs; ++r)
		{
			const auto& run = store->run(r);
			if (!run.is_current() || run.model != faces::model_id()) continue;
			for (uint32_t i = 0; i < run.count; ++i)
			{
				const auto f = run.first + i;
				if (!store->entry(f).has_hash) continue;
				const auto found = signatures.find(store->signature(f));
				if (found == signatures.end()) continue;
				auto& m = by_path[run.path].emplace_back(found->second);
				m.box = store->entry(f).bounds();
				m.position = store->entry(f).position;
			}
		}
	}
	for (const auto& [path, marks] : by_path)
	{
		if (marks.empty()) continue;
		const auto md = index.find_item(path).metadata.load();
		if (!md) continue;

		const auto regions = faces::unpack_regions(md->face_regions, md->dimensions(), md->orientation);
		std::vector<faces::found_face> found(marks.size());
		for (size_t m = 0; m < marks.size(); ++m) found[m].box = marks[m].box;
		const auto matched = faces::match_regions(regions, found);

		face_name_edits edits;
		edits.stored = md->dimensions();
		edits.orientation = md->orientation;

		for (size_t m = 0; m < marks.size(); ++m)
		{
			const auto& mark = marks[m];
			std::string current;
			for (size_t k = 0; k < regions.size(); ++k)
				if (matched[k] == static_cast<int>(m)) current = regions[k].name;

			if (mark.hidden) continue;
			std::optional<std::string> wanted;
			if (const auto* person = roster->person(mark.person)) wanted = person->name;
			else if (const auto* named = roster->named(current);
			         named && std::ranges::find(mark.rejected, named->id) != mark.rejected.end())
				wanted = std::string{};

			if (!wanted) continue;

			if (!wanted->empty() && core::icmp(current, *wanted) != 0)
			{
				edits.edits.push_back({face_name_edit::kind::name, mark.box, *wanted, {}});
				++plan.named;
			}
			else if (wanted->empty() && !current.empty())
			{
				edits.edits.push_back({face_name_edit::kind::remove, mark.box, current, {}});
				++plan.removed;
			}
		}

		if (!edits.edits.empty()) plan.edits.emplace(path, std::move(edits));
	}

	return plan;
}

void faces_view::save_names()
{
	_state.queue_async(async_queue::sidebar, [view = ui_owned(_state._async, shared_from_this()),
	                                          &index = _state.item_index, &async = _state._async]
	{
		auto plan = plan_name_writes(index, index.faces().book());
		async.queue_ui([view, plan = std::move(plan)]() mutable { view->names_planned(std::move(plan)); });
	});
}

void faces_view::names_planned(face_name_plan plan)
{
	if (plan.edits.empty())
	{
		make_dlg(_host->owner())
			->show_message(icon_index::person, tt.command_face_save_names, tt.faces_save_names_none);
		return;
	}

	// docs/faces.md#10-saving-names-to-files: what changes in each picture, listed before anything is written - its name
	// and each name it gains or loses.
	std::vector<std::pair<core::file_path, std::string>> lines;
	lines.reserve(plan.edits.size());
	for (const auto& [path, edits] : plan.edits)
	{
		std::string line;
		for (const auto& e : edits.edits)
		{
			if (!line.empty()) line += ", ";
			line += e.what == face_name_edit::kind::remove ? "\u2212 " : "+ ";
			line += e.name;
		}
		lines.emplace_back(path, std::move(line));
	}
	std::ranges::sort(lines, [](const auto& a, const auto& b) { return a.first.icmp(b.first) < 0; });

	constexpr size_t listed = 12;
	std::string list;
	for (size_t i = 0; i < std::min(listed, lines.size()); ++i)
	{
		if (!list.empty()) list += '\n';
		list += std::format("{}: {}", lines[i].first.name(), lines[i].second);
	}
	if (lines.size() > listed)
		list += "\n" + format_plural_text(tt.faces_more_pictures_fmt, static_cast<int64_t>(lines.size() - listed));

	const auto pictures = format_plural_text(tt.faces_pictures_fmt, static_cast<int64_t>(plan.edits.size()));
	const auto named = format_plural_text(tt.faces_named_fmt, static_cast<int64_t>(plan.named));
	const auto taken_out = format_plural_text(tt.faces_taken_out_fmt, static_cast<int64_t>(plan.removed));
	const auto text = plan.named && plan.removed
	                      ? str_format(tt.faces_save_names_plan_fmt, pictures, named, taken_out)
	                      : str_format(tt.faces_save_names_plan_one_fmt, pictures, plan.named ? named : taken_out);

	const auto dlg = make_dlg(_host->owner());
	const std::vector<view_element_ptr> controls = {
		set_margin(
			std::make_shared<ui::title_control2>(dlg->_frame, icon_index::save, tt.command_face_save_names, text)),
		std::make_shared<divider_element>(),
		set_margin(std::make_shared<text_element>(list)),
		std::make_shared<divider_element>(),
		std::make_shared<ui::ok_cancel_control>(dlg->_frame, tt.command_face_save_names),
	};
	if (dlg->show_modal(controls) != ui::close_result::ok) return;
	write_face_edits(std::move(plan.edits), tt.command_face_save_names, std::string(tt.command_face_save_names));
}

void faces_view::write_face_edits(face_edits_by_path by_path, const std::string_view title, std::string what)
{
	df::item_elements items;
	for (const auto& [path, edits] : by_path)
		items.push_back(std::make_shared<df::item_element>(path, _state.item_index.find_item(path)));

	batch_edit_spec spec;
	spec.make_edits = [by_path = std::move(by_path)](const core::file_path path) -> std::optional<item_edits>
	{
		const auto found = by_path.find(path);
		if (found == by_path.end()) return {};
		item_edits result;
		result.metadata.faces = found->second;
		return result;
	};
	spec.process_type = df::process_items_type::can_save_metadata;
	spec.encode_params.jpeg_save_quality = setting.jpeg_save_quality;
	spec.change_icon = icon_index::person;
	spec.change_title = std::move(what);

	const auto dlg = make_dlg(_host->owner());
	const auto results = std::make_shared<command_status>(_state._async, dlg, icon_index::person, title, items.size());
	_state.modify_items(results, items, spec, _host);
}

// A person files name whom face search has found no face of is renamed in every file naming them.
void faces_view::rename_file_person(const int row)
{
	const auto* const person = _model ? _model->find_person(_rows[row].title) : nullptr;
	if (!person) return;

	// Taken now: the model can be made again while the name is being typed.
	const auto from = person->name;
	std::vector<core::file_path> paths;
	paths.reserve(person->named.size());
	for (const auto& named : person->named) paths.push_back(named.path);

	auto to = from;
	if (!ask_name(tt.command_face_rename_person, str_format(tt.face_rename_fmt, from), to) || to.empty() || to == from)
		return;

	const auto& book = _state.item_index.faces().book();
	if (_book_generation != book.generation()) return;
	if (offer_combine(from, to)) return;

	face_edits_by_path by_path;
	for (const auto path : paths)
	{
		auto& edits = by_path[path];
		if (edits.edits.empty()) edits.edits.push_back({face_name_edit::kind::rename, {}, from, to});
	}

	const auto pictures = static_cast<int64_t>(by_path.size());
	if (pictures > 1 && !ask_to_confirm(_host->owner(), icon_index::person, tt.command_face_rename_person,
	                                    str_format(tt.face_rename_confirm_fmt, from, to,
	                                               format_plural_text(tt.faces_pictures_fmt, pictures)),
	                                    tt.button_ok))
		return;

	_open_person = to;
	write_face_edits(std::move(by_path), tt.command_face_rename_person,
	                 std::format("{}: {} \u2192 {}", tt.command_face_rename_person, from, to));
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The face book in a file: docs/faces.md#65-saving-and-moving-the-face-book
///////////////////////////////////////////////////////////////////////////////////////////////////

void export_face_book(view_state& state, const core::file_path path, std::function<void(bool, std::string)> done)
{
	state.queue_async(async_queue::work, [&state, path, done = std::move(done)]
	{
		auto& search = state.item_index.faces();
		search.bind_legacy_signatures();
		const auto& book = search.book();
		const auto generation = book.generation();
		auto roster = *book.people();
		auto signatures = book.all_signatures();
		auto skipped = book.unportable_count();
		const auto overview = search.current_overview();
		const auto store = search.store();
		// Include faces confirmed by the files themselves without silently copying their metadata into the live book.
		if (store && (!overview || overview->store != store || overview->generation != store->generation() ||
		              overview->book_generation != generation))
		{
			state.queue_ui([done] { done(false, std::string(tt.faces_export_wait)); });
			return;
		}
		if (overview && store)
		{
			std::map<faces::face_signature, uint32_t> from_files;
			for (const auto& p : overview->people)
			{
				uint32_t id = p.id;
				for (const auto& m : p.confirmed)
				{
					if (m.kind != faces::member_kind::named) continue;
					if (!store->entry(m.face).has_hash)
					{
						++skipped;
						continue;
					}
					if (!id)
					{
						if (const auto* existing = roster.named(p.name)) id = existing->id;
						else
						{
							id = roster.people.empty() ? 1 : roster.people.back().id + 1;
							if (!id)
							{
								++skipped;
								continue;
							}
							roster.people.push_back({id, p.name});
						}
					}
					const auto [it, added] = from_files.try_emplace(store->signature(m.face), id);
					if (!added && it->second != id) it->second = 0; // Conflicting file names cannot be a portable fact.
				}
			}
			for (const auto& [key, id] : from_files)
			{
				if (!id)
				{
					++skipped;
					continue;
				}
				auto& evidence = signatures[key];
				if (!evidence.person && !evidence.hidden && !std::ranges::binary_search(evidence.rejected, id))
					evidence.person = id;
			}
		}
		if (book.generation() != generation)
		{
			state.queue_ui([done] { done(false, std::string(tt.faces_export_wait)); });
			return;
		}
		const auto people = roster.people.size();
		const auto marks = signatures.size();
		faces::face_book snapshot;
		snapshot.load(std::move(roster), {}, std::move(signatures));
		const auto csv = snapshot.to_csv();
		// Written beside the file and put in its place, so a failed export leaves the last one whole.
		const auto ok = save_file_replacing(path, {std::bit_cast<const uint8_t*>(csv.data()), csv.size()}).empty();

		auto message = ok ? str_format(tt.faces_exported_fmt,
		                               format_plural_text(tt.faces_people_fmt, static_cast<int64_t>(people)),
		                               format_plural_text(tt.faces_decisions_fmt, static_cast<int64_t>(marks)),
		                               path.str())
		                  : str_format(tt.faces_export_failed_fmt, path.str());
		if (ok && skipped) message += " " + str_format(tt.faces_export_skipped_fmt, skipped);
		state.queue_ui([done, ok, message = std::move(message)] { done(ok, message); });
	});
}

void import_face_book(view_state& state, const core::file_path path, std::function<void(bool, std::string)> done)
{
	state.queue_async(async_queue::work, [&state, path, done = std::move(done)]
	{
		const auto text = read_text_file(path, max_face_book_bytes);
		faces::import_result result;
		if (text.empty()) result.error = "the file could not be read";
		else result = state.item_index.faces().book().import_csv(text);

		auto message = result.ok
		                   ? str_format(tt.faces_imported_fmt,
		                                format_plural_text(tt.faces_people_fmt,
		                                                   static_cast<int64_t>(result.people_added +
		                                                                        result.people_joined)),
		                                format_plural_text(tt.faces_decisions_fmt, static_cast<int64_t>(result.faces)),
		                                path.str())
		                   : str_format(tt.faces_import_failed_fmt, path.str(), result.error);
		state.queue_ui([done, ok = result.ok, message = std::move(message)] { done(ok, message); });
	});
}

static std::vector<platform::file_dialog_filter> face_data_filters()
{
	return {{std::string(tt.faces_data_files.sv()), "*.csv"}};
}

void faces_view::export_data()
{
	auto path = core::folder_path(platform::known_path(platform::known_folder::documents))
	                .combine_file_ext("Diffractor faces", ".csv");
	if (!platform::prompt_for_save_path(path, face_data_filters())) return;

	export_face_book(_state, path, [owner = _host->owner()](const bool ok, const std::string& message)
	{ make_dlg(owner)->show_message(ok ? icon_index::save : icon_index::error, tt.command_face_export, message); });
}

void faces_view::import_data()
{
	std::vector<core::file_path> paths;
	if (!platform::prompt_for_open_paths(paths, face_data_filters(), false) || paths.empty()) return;

	import_face_book(_state, paths.front(), [owner = _host->owner()](const bool ok, const std::string& message)
	{ make_dlg(owner)->show_message(ok ? icon_index::import : icon_index::error, tt.command_face_import, message); });
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Face search's progress
///////////////////////////////////////////////////////////////////////////////////////////////////

void faces_view::update_status()
{
	const auto& faces = _state.item_index.faces();
	const auto progress = faces.progress();
	std::string text;

	if (!faces.is_enabled())
	{
		text = tt.face_search_off;
	}
	else if (faces.is_paused())
	{
		text = str_format(tt.face_search_paused_fmt, progress.done, count_read(faces, progress.eligible));
	}
	else if (progress.eligible > 0 && progress.left() > 0)
	{
		text = str_format(tt.face_search_progress_fmt, progress.done, count_read(faces, progress.eligible),
		                  count_faces(progress.faces), about_how_long(progress.left(), 0, progress.rate));
	}
	else if (progress.eligible > 0)
	{
		text = str_format(tt.face_search_done_fmt, count_read(faces, progress.eligible), count_faces(progress.faces));
	}
	else
	{
		text = tt.faces_reading;
	}

	// Where videos are read too, what could not be read is a file rather than a picture.
	const auto files = faces.reads_videos();
	if (progress.unreadable > 0)
		text += " " + format_plural_text(files ? tt.face_search_unreadable_files_fmt : tt.face_search_unreadable_fmt,
		                                 static_cast<int64_t>(progress.unreadable));
	if (progress.waiting > 0)
		text += " " + format_plural_text(files ? tt.face_search_waiting_files_fmt : tt.face_search_waiting_fmt,
		                                 static_cast<int64_t>(progress.waiting));
	// docs/faces.md#9-the-faces-view: the suggestions are made again after every change, which on a large collection takes
	// a moment; while they are, it says so, so nothing on screen is mistaken for the last word.
	if (faces.is_enabled() && faces.is_grouping()) text += " " + std::string(tt.faces_updating);
	// docs/faces.md#64-the-face-database: why nothing here can be changed, or why a change is not saved yet.
	if (faces.book_state() == faces::book_keeping::shared) text += " " + std::string(tt.faces_read_only);
	if (faces.book_state() == faces::book_keeping::unsaved) text += " " + std::string(tt.faces_not_saved_yet);

	if (const auto check = _photos_check.lock()) check->checked(faces.reads_photos());
	else _scan_photos = faces.reads_photos();
	if (const auto check = _videos_check.lock()) check->checked(faces.reads_videos());
	else _scan_videos = faces.reads_videos();
	if (_progress_text) _progress_text->text(text);
	if (_pause_button) _pause_button->window_text(faces.is_paused() ? tt.face_search_resume : tt.face_search_pause);
	if (_book_text)
	{
		switch (faces.book_state())
		{
		case faces::book_keeping::kept:
		case faces::book_keeping::opening:
			_book_text->text(str_format(tt.faces_book_kept_fmt, faces.book_path().folder().text()));
			break;
		case faces::book_keeping::unsaved: _book_text->text(tt.faces_book_unsaved); break;
		case faces::book_keeping::shared: _book_text->text(tt.faces_book_shared); break;
		case faces::book_keeping::memory: _book_text->text(tt.faces_book_not_kept); break;
		}
	}
	if (_delete_button) _delete_button->enable(faces.can_change_book());

	if (_pause_button) _pause_button->is_visible(faces.is_enabled() && (faces.is_paused() || progress.left() > 0));
	_status = std::move(text);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Turning face search on and off: docs/faces.md#31-turning-it-on-and-off
///////////////////////////////////////////////////////////////////////////////////////////////////

bool turn_on_face_search(view_state& state, const ui::frame_ptr& parent, const bool photos, const bool videos)
{
	auto& faces = state.item_index.faces();
	if (!photos && !videos)
	{
		turn_off_face_search(state);
		return true;
	}

	// What it does not read yet is asked about first; a kind turned off, or one read already, asks nothing.
	const auto adds_photos = photos && !faces.reads_photos();
	const auto adds_videos = videos && !faces.reads_videos();

	if (adds_photos || adds_videos)
	{
		const auto& stats = state.item_index.stats;
		const auto picture_count = adds_photos ? static_cast<uint32_t>(std::max(0, stats.picture_item_count)) : 0u;
		const auto video_count = adds_videos ? static_cast<uint32_t>(std::max(0, stats.video_item_count)) : 0u;
		const auto pictures = format_plural_text(tt.faces_pictures_fmt, static_cast<int64_t>(picture_count));
		const auto videos_counted = format_plural_text(tt.faces_videos_fmt, static_cast<int64_t>(video_count));
		const auto how_long = about_how_long(picture_count, video_count, 0.0);
		// A kind the collection holds none of goes unmentioned: a collection of photos alone is not told of videos.
		const auto mention_photos = adds_photos && (picture_count > 0 || video_count == 0);
		const auto mention_videos = adds_videos && (video_count > 0 || picture_count == 0);
		const auto read = !mention_videos ? str_format(tt.face_search_read_fmt, pictures, how_long)
		                  : !mention_photos
		                      ? str_format(tt.face_search_read_videos_fmt, videos_counted, how_long)
		                      : str_format(tt.face_search_read_both_fmt, pictures, videos_counted, how_long);

		const auto dlg = make_dlg(parent);
		const std::vector<view_element_ptr> controls = {
			set_margin(std::make_shared<ui::title_control2>(dlg->_frame, icon_index::person, tt.face_scan,
		                                                    tt.face_search_local)),
			std::make_shared<divider_element>(),
			set_margin(std::make_shared<text_element>(read)),
			set_margin(std::make_shared<text_element>(tt.face_search_kept)),
			std::make_shared<divider_element>(),
			std::make_shared<ui::ok_cancel_control>(dlg->_frame, tt.face_scan),
		};

		if (dlg->show_modal(controls) != ui::close_result::ok) return false;
	}

	setting.faces.photos = photos;
	setting.faces.videos = videos;
	setting.faces.paused = false;
	faces.pause(false);
	faces.turn_on(photos, videos);
	state.invalidate_view(view_invalid::options_save | view_invalid::sidebar | view_invalid::command_state);
	return true;
}

void turn_off_face_search(view_state& state)
{
	setting.faces.photos = false;
	setting.faces.videos = false;
	state.item_index.faces().turn_off();
	state.invalidate_view(view_invalid::options_save | view_invalid::sidebar | view_invalid::command_state);
}

void pause_face_search(view_state& state, const bool paused)
{
	setting.faces.paused = paused;
	state.item_index.faces().pause(paused);
	state.invalidate_view(view_invalid::options_save | view_invalid::sidebar);
}

namespace
{
	// A dialog whose action cannot be undone, or only by hours of reading again: Cancel is its default, so Enter
	// does nothing it cannot take back. `extra` makes what the dialog shows between its text and its buttons.
	bool ask_before_removing(const ui::frame_ptr& parent, const std::string_view title, const std::string_view text,
	                         const std::string_view action,
	                         const std::function<std::vector<view_element_ptr>(const ui::frame_ptr&)>& extra = {})
	{
		const auto dlg = make_dlg(parent);
		const auto frame = dlg->_frame;
		std::vector<view_element_ptr> controls = {
			set_margin(std::make_shared<ui::title_control2>(frame, icon_index::del, title, text)),
			std::make_shared<divider_element>(),
		};
		if (extra)
		{
			for (auto& e : extra(frame)) controls.emplace_back(set_margin(std::move(e)));
			controls.emplace_back(std::make_shared<divider_element>());
		}
		controls.emplace_back(std::make_shared<ui::button_row>(
			std::vector<ui::control_ptr>{ui::make_button(action, [frame] { frame->close(false); }),
		                                 ui::make_button(tt.button_cancel, [frame] { frame->close(true); }, true)}));
		return dlg->show_modal(controls) == ui::close_result::ok;
	}
}

// docs/faces.md#31-turning-it-on-and-off: scan results can be discarded independently of the decisions kept in the
// book, and forgetting the book - which nothing brings back - is asked about again, saying what goes.
bool delete_face_data(view_state& state, const ui::frame_ptr& parent)
{
	auto& faces = state.item_index.faces();
	if (!faces.can_change_book()) return false;
	const auto read = faces.progress().done;
	const auto text = read > 0 ? str_format(tt.face_search_delete_fmt, count_read(faces, read))
	                           : std::string(tt.face_search_delete_all);
	auto forget = false;

	if (!ask_before_removing(parent, tt.face_search_delete, text, tt.button_delete, [&forget](const ui::frame_ptr& f)
	{ return std::vector<view_element_ptr>{std::make_shared<ui::check_control>(f, tt.face_search_forget, forget)}; }))
		return false;

	auto& book = faces.book();
	const auto people = book.people()->people.size();
	const auto decisions = book.mark_count();
	if (forget && (people || decisions))
	{
		const auto what = str_format(tt.faces_forget_fmt,
		                             format_plural_text(tt.faces_people_fmt, static_cast<int64_t>(people)),
		                             format_plural_text(tt.faces_decisions_fmt, static_cast<int64_t>(decisions)));
		if (!ask_before_removing(parent, tt.faces_forget_title, what, tt.faces_forget)) return false;
	}

	setting.faces.photos = false;
	setting.faces.videos = false;
	setting.faces.paused = false;
	faces.delete_data();
	if (forget) book.clear();
	state.invalidate_view(view_invalid::options_save | view_invalid::sidebar | view_invalid::command_state);
	return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The panel
///////////////////////////////////////////////////////////////////////////////////////////////////

void faces_view::update_selection()
{
	_confirm_label.clear();
	_remove_label.clear();
	_selection_detail = tt.faces_review_hint;
	if (faces_targeted())
	{
		_selection_title = format_plural_text(tt.faces_selected_fmt, _face_selection.size());
		if (!_review_person.empty())
		{
			_confirm_label = str_format(tt.faces_confirm_person_fmt, _review_person);
			_remove_label = str_format(tt.faces_not_person_fmt, _review_person);
		}
	}
	else
	{
		_selection_title = _faces_title;
		const auto rows = selected_rows();
		if (rows.size() == 1 && _rows[rows.front()].what == row::kind::file_person)
			_selection_detail = tt.faces_file_person_hint;
	}
	if (_placement_edit && !can_undo_placement())
	{
		_placement_edit.reset();
		_before_placement.clear();
	}
}

view_controls_host_ptr faces_view::controls(const ui::frame_ptr& owner)
{
	auto result = std::make_shared<view_controls_host>(_state);
	_scan_photos = _state.item_index.faces().reads_photos();
	_scan_videos = _state.item_index.faces().reads_videos();

	_progress_text = std::make_shared<text_element>(std::string_view{});
	_book_text = std::make_shared<text_element>(std::string_view{});
	_pause_button = ui::make_button(tt.face_search_pause, [this]
	{
		pause_face_search(_state, !_state.item_index.faces().is_paused());
		update_status();
	});

	std::vector<view_element_ptr> controls;
	_panel_commands.clear();
	const auto command_button = [this](const commands id, const std::string_view label)
	{
		auto button = ui::make_button(label, [this, id] { _state.invoke(id); });
		_panel_commands.emplace_back(id, button);
		return button;
	};
	// docs/faces.md#9-the-faces-view: what the view is for and that confirming teaches it, then scanning, then the
	// backup, and last what removes face data. Saving names to files is not offered in this release.
	controls.emplace_back(set_margin(std::make_shared<ui::title_control>(icon_index::person, tt.command_faces)));
	controls.emplace_back(set_margin(std::make_shared<text_element>(tt.faces_review_hint)));
	controls.emplace_back(set_margin(std::make_shared<text_element>(tt.faces_learns)));
	// docs/faces.md#31-turning-it-on-and-off: photos and videos are read apart, and neither is face search off.
	const auto scan_check = [this, owner](const std::string_view label, bool& value, const bool video)
	{
		return std::make_shared<ui::check_control>(owner, label, value, false, false,
		                                           [this, owner, video](const bool checked)
		{
			const auto& faces = _state.item_index.faces();
			turn_on_face_search(_state, owner, video ? faces.reads_photos() : checked,
			                    video ? checked : faces.reads_videos());
			update_status();
			_state.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw);
		});
	};
	const auto photos_check = scan_check(tt.face_scan_photos, _scan_photos, false);
	const auto videos_check = scan_check(tt.face_scan_videos, _scan_videos, true);
	_photos_check = photos_check;
	_videos_check = videos_check;
	controls.emplace_back(set_margin(photos_check));
	controls.emplace_back(set_margin(videos_check));
	controls.emplace_back(set_margin(_progress_text));
	controls.emplace_back(set_margin(_pause_button));
	controls.emplace_back(std::make_shared<divider_element>());
	controls.emplace_back(set_margin(std::make_shared<ui::title_control>(icon_index::import, tt.faces_backup)));
	controls.emplace_back(set_margin(std::make_shared<text_element>(tt.faces_backup_info)));
	controls.emplace_back(set_margin(command_button(commands::face_export, tt.command_face_export)));
	controls.emplace_back(set_margin(command_button(commands::face_import, tt.command_face_import)));
	controls.emplace_back(set_margin(std::make_shared<text_element>(tt.faces_export_privacy)));
	controls.emplace_back(set_margin(_book_text));
	controls.emplace_back(std::make_shared<divider_element>());
	_delete_button = ui::make_button(tt.face_search_delete, [this, owner]
	{
		delete_face_data(_state, owner);
		update_status();
	});
	controls.emplace_back(set_margin(_delete_button));
	update_selection();
	update_status();
	result->_controls = std::move(controls);
	result->_frame = owner;
	return result;
}
