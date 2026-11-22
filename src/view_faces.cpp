// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The Faces view's people and inboxes, surname headings, review grid, pointer and keyboard selection,
// and visible crop queue. Review actions are in view_faces_actions.cpp. Owned by docs/faces.md#9-the-faces-view.

#include "pch.h"
#include "view_faces.h"

#include "app_commands.h"
#include "app_text.h"
#include "files.h"
#include "model_index.h"
#include "model_location.h"
#include "view_list.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// The model
///////////////////////////////////////////////////////////////////////////////////////////////////

const faces_view_model::person* faces_view_model::find_person(const std::string_view name) const
{
	const auto found = std::ranges::find_if(people, [name](const person& p) { return core::icmp(p.name, name) == 0; });
	return found != people.end() ? &*found : nullptr;
}

faces_view_model_ptr make_faces_view_model(const index_state& index, faces::face_store_const_ptr store,
                                           faces::overview_ptr overview, const std::function<bool()>& cancel)
{
	auto result = std::make_shared<faces_view_model>();
	result->store = std::move(store);
	if (overview && overview->store == result->store) result->overview = std::move(overview);

	struct gathered
	{
		core::hash_map<uint32_t, int> spellings;
		uint32_t pictures = 0;
		std::vector<faces_view_model::shown> named;
	};

	core::hash_map<std::string, gathered, core::ihash, core::ieq> people;
	const auto& store_ref = result->store;
	const auto roster = index.faces().book().people();
	auto until_cancel_check = 1024;
	auto cancelled = false;

	// The names pictures' metadata gives, where its regions place them.
	index.visit_collection_pictures(true, false, [&](const core::file_path path, const df::index_file_item& file, bool)
	{
		if (cancelled) return;
		if (--until_cancel_check == 0)
		{
			until_cancel_check = 1024;
			cancelled = cancel();
		}

		const auto md = file.metadata.load();
		const auto run = store_ref ? store_ref->find(path) : std::nullopt;

		if (!md || prop::is_null(md->people)) return;

		const auto regions = faces::unpack_regions(md->face_regions, md->dimensions(), md->orientation);

		// The face each region is, where face search has read the picture.
		std::vector<int> matched(regions.size(), -1);
		uint32_t first_face = 0;

		if (run)
		{
			const auto& found_run = store_ref->run(*run);
			std::vector<faces::found_face> found(found_run.count);
			for (uint32_t i = 0; i < found_run.count; ++i)
				found[i].box = store_ref->entry(found_run.first + i).bounds();
			matched = faces::match_regions(regions, found);
			first_face = found_run.first;
		}

		// Joined as tags are, so split as the index summary splits them.
		core::hash_set<std::string_view, core::ihash, core::ieq> counted;
		core::split2(md->people, true, [&](const std::string_view part)
		{
			const auto name = core::trim(part);
			if (name.empty()) return;

			const auto canonical = roster->canonical_name(name);
			const auto cached = core::cache(canonical);
			auto& g = people[std::string(canonical)];
			const auto before = g.named.size();
			auto has_region = false;

			for (size_t k = 0; k < regions.size(); ++k)
			{
				const auto& region = regions[k];
				if (!region.has_area() || core::icmp(region.name, name) != 0) continue;

				faces_view_model::shown s;
				s.path = path;
				s.area = region.area;
				has_region = true;
				if (matched[k] >= 0)
				{
					s.face = first_face + static_cast<uint32_t>(matched[k]);
					// Don't recreate a file-only person from a face the book rejected, moved away or hid.
					if (const auto& o = result->overview; o && o->store == store_ref)
					{
						const auto* person = o->person_of_face(*s.face);
						const auto* member = o->member_of_face(*s.face);
						if (!person || !member || member->kind == faces::member_kind::suggested ||
						    core::icmp(roster->canonical_name(person->name), canonical) != 0)
							continue;
					}
				}
				g.named.push_back(s);
			}

			if (!has_region && (!result->overview || !result->overview->file_name_contradicted(path, name)))
				g.named.push_back({{}, path, {}});
			if (g.named.size() > before)
			{
				if (counted.insert(cached.sv()).second) ++g.pictures;
				++g.spellings[cached.id];
			}
		});
	});

	if (cancelled) return nullptr;

	result->people.reserve(people.size());

	for (auto& [key, g] : people)
	{
		if (!g.pictures) continue;
		faces_view_model::person p;
		auto best_count = 0;
		auto best = core::cached{};

		for (const auto& [id, count] : g.spellings)
		{
			if (count > best_count || (count == best_count && id < best.id))
			{
				best = core::cached{id};
				best_count = count;
			}
		}

		p.name = best.is_empty() ? key : std::string(best.sv());
		p.pictures = g.pictures;

		// Regions first, the largest leading, so the person is shown by the face they are clearest in.
		std::ranges::stable_sort(g.named, [](const faces_view_model::shown& a, const faces_view_model::shown& b)
		{ return a.area.Width * a.area.Height > b.area.Width * b.area.Height; });
		p.named = std::move(g.named);
		if (!p.named.empty()) p.face = p.named.front();
		result->people.push_back(std::move(p));
	}

	std::ranges::sort(result->people, [](const faces_view_model::person& a, const faces_view_model::person& b)
	{
		if (a.pictures != b.pictures) return a.pictures > b.pictures;
		return core::icmp(a.name, b.name) < 0;
	});

	return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Crops
///////////////////////////////////////////////////////////////////////////////////////////////////

// The crops the view wants, most wanted first: a plan replaced whenever what is in view changes, so a crop scrolled
// away before its turn is never cut and the workers always cut what is wanted most. One picture is read at a time, so
// reads stay in order on the disk while the workers decode.
class face_crop_queue final : public std::enable_shared_from_this<face_crop_queue>
{
public:
	struct job
	{
		struct face
		{
			std::string key;
			rectd area;
			// In a video, the frame it was found in.
			uint32_t frame_ms = 0;
		};

		core::file_path path;
		file_type_ref ft = nullptr;
		int edge = 0;
		std::vector<face> faces;
		bool whole_picture = false;
		bool video = false;
	};

	using cut_crops = std::vector<std::pair<std::string, core::const_surface_ptr>>;
	using deliver_func = std::function<void(uint64_t generation, int edge, cut_crops cut)>;

private:
	async_strategy& _async;
	deliver_func _deliver;
	core::mutex _rw;
	_Guarded_by_(_rw) std::deque<job> _jobs;
	_Guarded_by_(_rw) core::hash_set<std::string> _cutting;
	_Guarded_by_(_rw) int _workers = 0;
	_Guarded_by_(_rw) uint64_t _generation = 0;
	core::mutex _reading;
	static constexpr int max_workers = 2;

	cut_crops cut(const job& j)
	{
		cut_crops result;

		try
		{
			thread_local std::unique_ptr<files> ff;
			if (!ff) ff = std::make_unique<files>();

			// The decode is as large as the smallest face needs to fill its square.
			auto smallest = 1.0;
			for (const auto& f : j.faces)
				smallest = std::min(smallest, f.area.Width > 0.0 ? std::max(f.area.Width, f.area.Height) * 1.6 : 1.0);
			const auto bound = std::clamp(static_cast<int>(std::ceil(j.edge / smallest)), j.edge,
			                              faces::defaults().decode_edge);
			const auto cut_from = [&j](const core::const_surface_ptr& decoded, const job::face& f)
			{
				return j.whole_picture || !decoded ? decoded : faces::cut_tile(decoded, f.area, j.edge);
			};

			if (j.video)
			{
				// docs/faces.md#33-videos: each face is cut from the frame of the video it was found in, each frame read
				// once and in turn, and decoded once it has been read.
				std::vector<uint32_t> frames;
				for (const auto& f : j.faces)
					if (std::ranges::find(frames, f.frame_ms) == frames.end()) frames.push_back(f.frame_ms);
				std::ranges::sort(frames);

				faces::face_frames read;
				{
					core::exclusive_lock lock(_reading);
					read = faces::read_face_frames(j.path, frames);
				}

				std::vector<core::const_surface_ptr> decoded(frames.size());
				for (const auto& f : j.faces)
				{
					const auto at = static_cast<size_t>(std::ranges::find(frames, f.frame_ms) - frames.begin());
					if (!decoded[at]) decoded[at] = faces::decode_face_frame(read, at, {bound, bound});
					result.emplace_back(f.key, cut_from(decoded[at], f));
				}
			}
			else
			{
				std::vector<uint8_t> owned;
				{
					core::exclusive_lock lock(_reading);
					const auto bytes = files::read_thumbnail_source(j.path, j.ft);
					owned.assign(bytes.begin(), bytes.end());
				}

				const auto decoded = faces::decode_for_faces(*ff, j.path, j.ft, owned, {bound, bound});
				for (const auto& f : j.faces) result.emplace_back(f.key, cut_from(decoded, f));
			}
		}
		catch (const std::exception& e)
		{
			core::log(__FUNCTION__, e.what());
		}

		// A picture that cut nothing is still answered, so its squares are not asked for again while they are near.
		for (const auto& f : j.faces)
		{
			if (std::ranges::none_of(result, [&f](const auto& c) { return c.first == f.key; }))
				result.emplace_back(f.key, nullptr);
		}

		return result;
	}

	void work()
	{
		for (;;)
		{
			job j;
			uint64_t generation = 0;
			{
				core::exclusive_lock lock(_rw);
				if (_jobs.empty() || platform::is_closing)
				{
					--_workers;
					return;
				}
				j = std::move(_jobs.front());
				_jobs.pop_front();
				generation = _generation;
				for (const auto& f : j.faces) _cutting.emplace(f.key);
			}

			auto crops = cut(j);

			{
				core::exclusive_lock lock(_rw);
				for (const auto& f : j.faces) _cutting.erase(f.key);
			}

			_deliver(generation, j.edge, std::move(crops));
		}
	}

public:
	face_crop_queue(async_strategy& async, deliver_func deliver) : _async(async), _deliver(std::move(deliver)) {}

	// Replaces what waits with `jobs`, most wanted first, leaving out the squares being cut now, and starts the
	// workers that cut them.
	void plan(std::vector<job> jobs, const uint64_t generation)
	{
		auto start = 0;
		{
			core::exclusive_lock lock(_rw);
			_jobs.clear();
			_generation = generation;

			for (auto& j : jobs)
			{
				std::erase_if(j.faces, [this](const auto& f) { return _cutting.contains(f.key); });
				if (!j.faces.empty()) _jobs.push_back(std::move(j));
			}

			start = std::min(max_workers - _workers, static_cast<int>(_jobs.size()));
			if (start < 0) start = 0;
			_workers += start;
		}

		for (auto i = 0; i < start; ++i)
			_async.queue_async(async_queue::thumbnail_decode, [self = shared_from_this()] { self->work(); });
	}

	void clear()
	{
		core::exclusive_lock lock(_rw);
		_jobs.clear();
	}

	size_t waiting()
	{
		core::exclusive_lock lock(_rw);
		return _jobs.size();
	}
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// The pointer
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	// A row of the People pane, or a face of the faces pane: clicked, double-clicked, or dragged.
	class faces_item_controller final : public view_controller
	{
		std::shared_ptr<faces_view> _view;
		faces_view::pane _pane;
		int _index = 0;
		bool _pressed = false;
		bool _dragging = false;
		bool _on_title = false;
		recti _title;
		pointi _last;
		std::optional<faces_view::suggestion_action> _review_action;
		std::string _person;
		faces_view_model::shown _face;
		bool _cancel_action = false;

	public:
		faces_item_controller(const view_host_ptr& host, std::shared_ptr<faces_view> view, const faces_view::pane pane,
		                      const int index, const recti bounds, const recti title,
		                      const std::optional<faces_view::suggestion_action> review_action = {}) :
			view_controller(host, bounds), _view(std::move(view)), _pane(pane), _index(index), _title(title),
			_review_action(review_action)
		{
			if (_review_action)
			{
				_face = _view->cells()[index].shown;
				_person = _view->faces_title();
			}
			if (_pane == faces_view::pane::groups) _view->hover_row(_index);
			else _view->hover_cell(_index);
		}

		~faces_item_controller() override
		{
			if (_dragging) _view->cancel_drag();
			if (_pressed) _view->release_rows();
			if (_pane == faces_view::pane::groups) _view->hover_row(-1);
			else _view->hover_cell(-1);
		}

		platform::cursor cursor() const override
		{
			if (_dragging) return platform::cursor::size_all;
			return platform::cursor::link;
		}

		void on_mouse_left_button_down(const pointi loc, const platform::key_state keys) override
		{
			view_controller::on_mouse_left_button_down(loc, keys);
			_cancel_action = false;
			_pressed = true;
			_view->hold_rows();
			_on_title = _title.contains(loc);
		}

		void on_mouse_move(const pointi loc) override
		{
			if (!_pressed) return;
			_last = loc;
			if (_review_action)
			{
				if (ui::is_drag(_start_loc, loc)) _cancel_action = true;
				return;
			}

			if (!_dragging && ui::is_drag(_start_loc, loc))
			{
				_dragging = true;
				_view->begin_drag(_pane, _index, loc);
			}

			if (_dragging) _view->drag_to(loc);
		}

		// A drag held still near the top or bottom of the People pane goes on carrying it.
		void tick() override
		{
			if (_dragging) _view->drag_to(_last);
		}

		void on_mouse_left_button_up(const pointi loc, const platform::key_state keys) override
		{
			const auto pressed = std::exchange(_pressed, false);
			const core::scope_exit release([&]
			{
				if (pressed) _view->release_rows();
			});

			if (std::exchange(_dragging, false))
			{
				_view->end_drag(loc);
				return;
			}

			if (!pressed || !_bounds.contains(loc)) return;
			if (_review_action && !keys.control && !keys.shift)
			{
				if (!_cancel_action) _view->review_suggestion(_index, _face, _person, *_review_action);
				return;
			}
			if (_pane == faces_view::pane::groups) _view->click_row(_index, keys, _on_title && _title.contains(loc));
			else _view->click_cell(_index, keys);
		}

		void on_mouse_left_button_double_click(const pointi /*loc*/, const platform::key_state /*keys*/) override
		{
			if (_review_action) return;
			if (_pane == faces_view::pane::groups) _view->double_click_row(_index);
			else _view->double_click_cell(_index);
		}

		bool escape() override
		{
			const auto pressed = std::exchange(_pressed, false);
			const auto dragging = std::exchange(_dragging, false);
			if (dragging) _view->cancel_drag();
			if (pressed) _view->release_rows();
			return pressed || dragging;
		}

		void popup_from_location(view_hover_element& hover) override
		{
			if (_dragging) return;
			if (_review_action)
			{
				using action = faces_view::suggestion_action;
				const auto what = *_review_action;
				hover.elements->add(std::make_shared<text_element>(_view->suggestion_action_text(what)));
				const auto hint = what == action::confirm  ? tt.faces_confirm_hint
				                  : what == action::reject ? tt.faces_reject_hint
				                                           : tt.faces_name_someone_else_hint;
				hover.elements->add(std::make_shared<text_element>(hint, render::font_face::dialog,
				                                                   render::text_style::multiline, flex_item::new_line));
				hover.active_bounds = hover.window_bounds = _bounds;
			}
			else if (_pane == faces_view::pane::groups) _view->bubble_row(_index, hover);
			else _view->bubble_cell(_index, hover);
		}
	};

	// A press and release on one thing: the faces pane's title, which renames, or the offer to turn face search on.
	class faces_press_controller final : public view_controller
	{
		std::function<void()> _invoke;
		bool _pressed = false;

	public:
		faces_press_controller(const view_host_ptr& host, const recti bounds, std::function<void()> invoke) :
			view_controller(host, bounds), _invoke(std::move(invoke))
		{
		}

		platform::cursor cursor() const override { return platform::cursor::link; }

		void on_mouse_left_button_down(const pointi loc, const platform::key_state keys) override
		{
			view_controller::on_mouse_left_button_down(loc, keys);
			_pressed = true;
		}

		void on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/) override
		{
			if (std::exchange(_pressed, false) && _bounds.contains(loc)) _invoke();
		}

		bool escape() override { return std::exchange(_pressed, false); }
	};

	// A button at the end of a heading: Confirm all, Name all or Hide all, acting on every face listed under it. It shows
	// it is pointed at, and its bubble says what it does.
	class faces_heading_button_controller final : public view_controller
	{
		std::shared_ptr<faces_view> _view;
		int _heading = -1;
		int _button = -1;
		bool _pressed = false;

	public:
		faces_heading_button_controller(const view_host_ptr& host, std::shared_ptr<faces_view> view, const recti bounds,
		                                const int heading, const int button) :
			view_controller(host, bounds), _view(std::move(view)), _heading(heading), _button(button)
		{
			_view->hover_heading_button(_heading, _button);
		}

		~faces_heading_button_controller() override { _view->hover_heading_button(-1, -1); }

		platform::cursor cursor() const override { return platform::cursor::link; }

		void on_mouse_left_button_down(const pointi loc, const platform::key_state keys) override
		{
			view_controller::on_mouse_left_button_down(loc, keys);
			_pressed = true;
		}

		void on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/) override
		{
			if (!std::exchange(_pressed, false) || !_bounds.contains(loc)) return;
			const auto& headings = _view->headings();
			if (_heading >= static_cast<int>(headings.size()) ||
			    _button >= static_cast<int>(headings[_heading].buttons.size()))
				return;
			_view->press_heading_button(_heading, headings[_heading].buttons[_button].action);
		}

		bool escape() override { return std::exchange(_pressed, false); }

		void popup_from_location(view_hover_element& hover) override
		{
			_view->bubble_heading_button(_heading, _button, hover);
			hover.active_bounds = hover.window_bounds = _bounds;
		}
	};

	// A press between faces. Dragged, it is a rubber band selecting the faces it touches, as Items' is, scrolling the
	// pane when held near its top or bottom; a click on a heading selects the faces under it, and a click anywhere
	// else between faces leaves the selection as it is.
	class faces_marquee_controller final : public view_controller
	{
		std::shared_ptr<faces_view> _view;
		int _heading = -1;
		bool _pressed = false;
		bool _banding = false;
		bool _toggle = false;
		pointi _last;

	public:
		faces_marquee_controller(const view_host_ptr& host, std::shared_ptr<faces_view> view, const recti bounds,
		                         const int heading) :
			view_controller(host, bounds), _view(std::move(view)), _heading(heading)
		{
			_view->hover_heading(_heading);
		}

		~faces_marquee_controller() override
		{
			if (_banding) _view->cancel_marquee();
			if (_pressed) _view->release_rows();
			_view->hover_heading(-1);
		}

		platform::cursor cursor() const override
		{
			if (_banding) return platform::cursor::select;
			return _heading >= 0 ? platform::cursor::link : platform::cursor::normal;
		}

		void on_mouse_left_button_down(const pointi loc, const platform::key_state keys) override
		{
			view_controller::on_mouse_left_button_down(loc, keys);
			_pressed = true;
			_view->hold_rows();
			_toggle = keys.control;
			_last = loc;
		}

		void on_mouse_move(const pointi loc) override
		{
			if (!_pressed) return;
			_last = loc;

			// The distance every press goes before it is a drag, so a click with a shaky hand stays a click.
			if (!_banding && ui::is_drag(_start_loc, loc))
			{
				_banding = true;
				_view->begin_marquee(_start_loc, _toggle);
			}

			if (_banding) _view->marquee_scroll(loc);
		}

		void on_mouse_left_button_up(const pointi loc, const platform::key_state keys) override
		{
			if (!std::exchange(_pressed, false)) return;
			if (std::exchange(_banding, false)) _view->end_marquee(loc);
			else if (_heading >= 0 && _bounds.contains(loc)) _view->click_heading(_heading, keys);
			_view->release_rows();
		}

		void tick() override
		{
			if (_banding) _view->marquee_scroll(_last);
		}

		bool escape() override
		{
			const auto was = std::exchange(_pressed, false);
			if (std::exchange(_banding, false)) _view->cancel_marquee();
			if (was) _view->release_rows();
			return was;
		}
	};

	// The strip between the groups and the faces: dragged, it shares the width between them.
	class faces_splitter_controller final : public view_controller
	{
		std::shared_ptr<faces_view> _view;
		bool _tracking = false;

	public:
		faces_splitter_controller(const view_host_ptr& host, std::shared_ptr<faces_view> view, const recti bounds) :
			view_controller(host, bounds), _view(std::move(view))
		{
		}

		~faces_splitter_controller() override
		{
			if (_tracking) _view->finish_groups_resize(false);
		}

		platform::cursor cursor() const override { return platform::cursor::left_right; }
		bool tracking() const { return _tracking; }

		void on_mouse_left_button_down(const pointi loc, const platform::key_state keys) override
		{
			view_controller::on_mouse_left_button_down(loc, keys);
			_tracking = true;
			_host->frame()->invalidate(_bounds);
		}

		void on_mouse_move(const pointi loc) override
		{
			if (_tracking) _view->groups_width(_bounds.right + loc.x - _start_loc.x);
		}

		void on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/) override
		{
			if (!_tracking) return;
			on_mouse_move(loc);
			_tracking = false;
			_view->finish_groups_resize(true);
		}

		bool escape() override
		{
			if (!_tracking) return false;
			_tracking = false;
			_view->finish_groups_resize(false);
			return true;
		}
	};

	std::string count_pictures(const uint32_t n)
	{
		return format_plural_text(tt.faces_pictures_fmt, static_cast<int64_t>(n));
	}

	std::string count_faces(const size_t n)
	{
		return format_plural_text(tt.faces_count_fmt, static_cast<int64_t>(n));
	}

	// docs/faces.md#91-people: the pictures a person is confirmed in, and how many faces wait for them to review.
	std::string person_detail(const faces::face_person& p)
	{
		auto detail = count_pictures(p.confirmed_pictures);
		if (!p.suggested.empty())
			detail = std::format("{} \u00b7 {}", detail,
			                     format_plural_text(tt.faces_suggested_count_fmt,
			                                        static_cast<int64_t>(p.suggested.size())));
		return detail;
	}

	// A name written in Chinese, Japanese or Korean characters, which puts the family name first.
	bool is_east_asian(const std::string_view word)
	{
		auto i = word.begin();
		const auto c = i == word.end() ? 0u : core::pop_utf8_char(i, word.end());
		return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x4DBF) ||
		       (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF);
	}

	// docs/faces.md#91-people: the family name a heading gathers people by - the last word of a name, before a suffix
	// such as Jr. or III, or the first of a Chinese, Japanese or Korean name written with a space. A name of one word,
	// or one written in those characters without a space, has none.
	std::string family_name(const std::string_view name)
	{
		std::vector<std::string_view> words;
		for (size_t start = 0; start < name.size();)
		{
			const auto end = std::min(name.find_first_of(" \t", start), name.size());
			if (end > start) words.push_back(name.substr(start, end - start));
			start = end + 1;
		}
		if (words.size() < 2) return {};
		if (is_east_asian(words.front())) return std::string(words.front());
		static constexpr std::array suffixes = {"jr"sv, "jr."sv, "sr"sv, "sr."sv, "ii"sv, "iii"sv, "iv"sv};
		while (words.size() > 1 &&
		       std::ranges::any_of(suffixes, [&](const auto s) { return core::icmp(words.back(), s) == 0; }))
			words.pop_back();
		if (words.size() < 2) return {};
		auto last = words.back();
		while (!last.empty() && last.back() == ',') last.remove_suffix(1);
		return std::string(last);
	}

	// Where a picture was taken, as the sidebar names it (docs/locations.md#item-locations): the place its file stores,
	// or else the place its coordinate is attributed to, which is looked up once and arrives later.
	std::string place_of(view_state& state, const prop::item_metadata& md)
	{
		if (!core::is_empty(md.location_place) || !core::is_empty(md.location_state) ||
		    !core::is_empty(md.location_country))
		{
			const location_t stored(0, md.location_place, md.location_state, md.location_country, {}, 0.0,
			                        static_cast<uint32_t>(location_qualification::name_region_country));
			return qualified_name(stored);
		}

		const auto* const derived = state.derived_location(md.coordinate);
		return derived ? attributed_name(*derived) : std::string{};
	}

}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The view
///////////////////////////////////////////////////////////////////////////////////////////////////

faces_view::faces_view(view_state& state, view_host_ptr host) : _state(state), _host(std::move(host))
{
	_clear_filter_link = std::make_shared<link_element>(tt.faces_clear_filter, [this] { filter_people({}); });
	_clear_filter_link->is_visible(false);
	_groups_scroller.changed_func = [this]
	{
		if (_active) request_crops();
	};
	_faces_scroller.changed_func = [this]
	{
		_pending_faces_anchor.reset();
		if (_active) request_crops();
	};
	_faces_scroller.popup_func = [this](view_hover_element& hover, const pointi loc)
	{
		faces_scroll_popup(hover, loc);
	};
}

faces_view::~faces_view()
{
	if (_crop_queue) _crop_queue->clear();
}

void faces_view::open(const std::string_view person)
{
	if (!person.empty())
	{
		_open_person = person;
		_filter.clear();
		if (_model) build_rows();
	}

	_state.open_in_history(view_type::faces);
}

void faces_view::open_at_face(const uint32_t face)
{
	_open_face = face;
	_filter.clear();
	if (_active && _model) build_rows();
	_state.open_in_history(view_type::faces);
}

void faces_view::filter_people(const std::string_view text)
{
	if (_filter == text) return;
	_filter = text;
	_filtered_collapsed.clear();
	_groups_scroller.reset();
	build_rows();
}

void faces_view::activate(const sizei extent)
{
	_active = true;
	_extent = extent;
	// Suggestions are made against the groups and names as they are now: grouped again if anything they are made
	// from changed since.
	_state.item_index.faces().queue_overview_if_stale();
	request_model();
	update_status();
}

void faces_view::deactivate()
{
	_active = false;
	++_model_request;
	_building = false;
	_build_again = false;
	_holding_rows = false;
	_rows_need_build = false;
	_pending_model.reset();
	_pending_faces_anchor.reset();
	_unknown_order = {};
	_reveal_face = false;
	_crops.clear();
	_preview_face = {};
	_pictures.clear();
	_bubble_picture_key.clear();
	++_crop_generation;
	if (_crop_queue) _crop_queue->clear();
	_hover = -1;
	_row_hover = -1;
	_drag = {};
	_panel_commands.clear();
	_delete_button.reset();
}

void faces_view::refresh()
{
	request_model();
}

void faces_view::model_changed()
{
	if (_active) request_model();
	update_status();
}

void faces_view::request_model()
{
	if (!_active) return;

	if (_building || _holding_rows || _drag.active)
	{
		_build_again = true;
		return;
	}

	_building = true;
	const auto request = ++_model_request;
	const auto& faces = _state.item_index.faces();
	auto store = faces.store();
	auto overview = faces.current_overview();

	_state.queue_async(async_queue::sidebar,
	                   [view = ui_owned(_state._async, shared_from_this()), &index = _state.item_index,
	                    &async = _state._async, store = std::move(store), overview = std::move(overview), request]
	{
		auto model = make_faces_view_model(index, store, overview, [] { return platform::is_closing.load(); });
		async.queue_ui([view, model = std::move(model), request]() mutable
		{ view->model_built(std::move(model), request); });
	});
}

void faces_view::model_built(faces_view_model_ptr model, const uint64_t request)
{
	_building = false;

	if (model && request == _model_request && _active)
	{
		if (_holding_rows || _drag.active)
		{
			_pending_model = std::move(model);
			return;
		}
		// A new store, after face search was turned off or on, numbers its faces afresh.
		if (!_model || _model->store != model->store)
		{
			_crops.clear();
			_preview_face = {};
			_bubble_picture_key.clear();
			_pictures.clear();
			++_crop_generation;
			_moved.clear();
			_before_placement.clear();
			_unknown_order = {};
		}

		_model = std::move(model);
		build_rows();
	}

	if (std::exchange(_build_again, false)) request_model();
}

void faces_view::faces_changed()
{
	update_status();
	if (!_active) return;

	const auto& faces = _state.item_index.faces();
	if (!_model || _model->store != faces.store() || _model->overview != faces.current_overview()) request_model();
	if (_model && _book_generation != faces.book().generation()) build_rows();
	_state.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw | view_invalid::command_state |
	                       view_invalid::status);
}

bool faces_view::is_current() const
{
	const auto& faces = _state.item_index.faces();
	return !_combining && !_building && !_build_again && !_pending_model && _model && _model->store == faces.store() &&
	       _model->overview == faces.current_overview() && _book_generation == faces.book().generation();
}

void faces_view::changed(const bool layout)
{
	update_selection();
	update_selection_marks();
	_state.invalidate_view((layout ? view_invalid::view_layout : view_invalid::none) | view_invalid::view_redraw |
	                       view_invalid::command_state | view_invalid::controller | view_invalid::status |
	                       view_invalid::tooltip);
}

faces_view_model::shown faces_view_model::shown::of(const faces::face_store& store, const uint32_t face)
{
	const auto& run = store.run_of_face(face);
	shown result{face, run.path, store.entry(face).bounds()};
	if (run.video) result.frame_ms = store.entry(face).time_ms;
	return result;
}

std::string faces_view::crop_key(const faces_view_model::shown& s)
{
	auto key = std::format("{}|{:.4f},{:.4f},{:.4f},{:.4f}", s.path.str(), s.area.X, s.area.Y, s.area.Width,
	                       s.area.Height);
	// Two faces of a video can stand in one place in two of its frames.
	if (s.frame_ms) key += std::format("@{}", *s.frame_ms);
	return key;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The People pane
///////////////////////////////////////////////////////////////////////////////////////////////////

int faces_view::find_row(const std::string_view key) const
{
	if (key.empty()) return -1;

	for (size_t i = 0; i < _rows.size(); ++i)
		if (_rows[i].key == key) return static_cast<int>(i);

	return -1;
}

void faces_view::build_rows()
{
	if (_holding_rows || _drag.active)
	{
		_rows_need_build = true;
		return;
	}
	_rows_need_build = false;

	std::string standing_row;
	int standing_y = 0;
	if (const auto top = _groups_scroller.scroll_offset().y; top > 0)
	{
		const auto focus = focus_row();
		const auto middle = top + _groups_scroller.client_bounds().height() / 2;
		const auto at = !row_bounds(focus).is_empty() ? _rows.begin() + focus
		                                              : std::ranges::find_if(_rows, [middle](const row& r)
		{ return !r.bounds.is_empty() && r.bounds.bottom > middle; });
		if (at != _rows.end())
		{
			standing_row = at->key;
			standing_y = at->bounds.top - top;
		}
	}

	_rows.clear();
	const auto& book = _state.item_index.faces().book();
	const auto roster = book.people();
	_book_generation = book.generation();
	const auto o = _model ? _model->overview : nullptr;
	if (o) std::erase_if(_moved, [&](const auto& m) { return m.second.generation <= o->book_generation; });

	const auto shown = [&](const uint32_t f) -> faces_view_model::shown
	{
		return faces_view_model::shown::of(*o->store, f);
	};
	const auto preview = [](row& r, const faces_view_model::shown& sample)
	{
		if (r.previews.size() == 3 || sample.path.is_empty()) return;
		if (std::ranges::any_of(r.previews, [&](const auto& s) { return s.path == sample.path; })) return;
		if (r.previews.empty()) r.face = sample;
		r.previews.push_back(sample);
	};
	const auto inbox = [&](const bool hidden)
	{
		row r;
		r.what = hidden ? row::kind::hidden_faces : row::kind::unknown;
		r.title = hidden ? tt.faces_hidden_section : tt.unknown;
		r.key = hidden ? "hidden" : "unknown";
		size_t n = 0;
		if (o)
		{
			const auto& list = hidden ? o->hidden : o->unknown;
			n = list.size();
			for (const auto f : hidden ? list : o->unknown_for_review())
			{
				if (const auto it = _moved.find(f); it == _moved.end() || it->second.row_key == r.key)
					preview(r, shown(f));
				if (r.previews.size() == 3) break;
			}
			for (const auto& [f, m] : _moved)
			{
				const auto was = std::ranges::binary_search(list, f);
				const auto now = m.row_key == r.key;
				if (was && !now) --n;
				if (!was && now) ++n;
				if (now) preview(r, shown(f));
			}
		}
		r.detail = count_faces(n);
		_rows.push_back(std::move(r));
	};
	inbox(false);
	inbox(true);

	core::hash_map<std::string, row, core::ihash, core::ieq> named;
	for (const auto& p : roster->people)
	{
		auto& r = named[p.name];
		r.title = p.name;
		r.book_person = p.id;
	}
	if (o)
	{
		for (size_t i = 0; i < o->people.size(); ++i)
		{
			const auto& p = o->people[i];
			const auto* kept = p.id ? roster->person(p.id) : nullptr;
			if (!kept && roster->aliases.contains(p.name)) kept = roster->named(p.name);
			if (p.id && !kept) continue; // Deleted by Undo, not combined into another identity.
			const auto& name = kept ? kept->name : p.name;
			auto& r = named[name];
			r.title = name;
			r.overview_people.push_back(static_cast<int>(i));
			r.detail = person_detail(p);
			for (const auto f : p.samples)
				if (!_moved.contains(f)) preview(r, shown(f));
		}
	}
	if (_model)
	{
		for (size_t i = 0; i < _model->people.size(); ++i)
		{
			const auto& p = _model->people[i];
			const auto canonical = roster->canonical_name(p.name);
			auto& r = named[std::string(canonical)];
			if (r.title.empty()) r.title = canonical;
			r.file_people.push_back(static_cast<int>(i));
			if (r.detail.empty()) r.detail = count_pictures(p.pictures);
			for (const auto& s : p.named)
			{
				if (!s.face || !_moved.contains(*s.face)) preview(r, s);
				if (r.previews.size() == 3) break;
			}
		}
	}
	// docs/faces.md#91-people: everyone filed by family name, or by their name where it has none; a family of two or
	// more under a heading that folds it, anyone else on their own.
	struct filed
	{
		std::string by;
		bool alone = true;
		row r;
	};
	std::vector<filed> people;
	core::hash_map<std::string, int, core::ihash, core::ieq> families;
	for (const auto& [name, r] : named)
		if (!name.empty())
			if (auto family = family_name(r.title); !family.empty()) ++families[family];
	for (auto& [name, r] : named)
	{
		if (name.empty() || (!_filter.empty() && !core::ifind2(name, _filter, 0).found)) continue;
		r.what = r.book_person ? row::kind::person : row::kind::file_person;
		r.key = r.book_person ? std::format("p:{}", r.book_person) : "n:" + core::to_lower(name);
		if (r.book_person) _people_selection.rename("n:" + core::to_lower(name), r.key);
		auto family = family_name(r.title);
		people.push_back({family.empty() ? r.title : family, true, std::move(r)});
		people.back().r.surname = std::move(family);
	}
	for (auto& p : people)
	{
		p.alone = p.r.surname.empty() || families[p.r.surname] < 2;
		if (p.alone) p.r.surname.clear();
	}
	std::ranges::sort(people, [](const filed& a, const filed& b)
	{
		if (const auto cmp = core::icmp(a.by, b.by)) return cmp < 0;
		if (a.alone != b.alone) return !a.alone;
		return core::icmp(a.r.title, b.r.title) < 0;
	});
	std::string last_surname;
	for (auto& p : people)
	{
		if (!p.alone && core::icmp(last_surname, p.r.surname) != 0)
		{
			last_surname = p.r.surname;
			row header;
			header.what = row::kind::surname;
			header.title = header.surname = last_surname;
			header.key = "s:" + core::to_lower(last_surname);
			_rows.push_back(std::move(header));
		}
		else if (p.alone) last_surname.clear();
		_rows.push_back(std::move(p.r));
	}
	std::string open;
	std::optional<uint32_t> focus_face;
	if (_open_face && o && o->book_generation == _book_generation && *_open_face < o->store->face_count())
	{
		const auto where = placement_of(*_open_face);
		open = where.offer_key.empty() ? where.row_key : where.offer_key;
		focus_face = std::exchange(_open_face, {});
	}
	if (!_open_person.empty())
	{
		for (const auto& r : _rows)
			if (r.named() && core::icmp(r.title, _open_person) == 0) open = r.key;
		if (!open.empty()) _open_person.clear();
	}
	if (const int at = find_row(open); at >= 0)
	{
		_collapsed_surnames.erase(_rows[at].surname);
		_people_selection.click(open, false, false, {});
	}
	std::vector<std::string> keys;
	for (const auto& r : _rows) keys.push_back(r.key);
	const auto selected = _people_selection.size();
	_people_selection.retain(keys);
	if (!open.empty() || _people_selection.size() != selected)
	{
		_face_selection.clear();
		_pending_faces_anchor.emplace();
		_unknown_order = {};
		_reveal_face = false;
		_faces_scroller.reset();
	}
	if (_people_selection.empty()) _people_selection.click(_rows.front().key, false, false, {});
	layout_rows(nullptr);
	if (!open.empty()) ensure_row_visible(focus_row());
	else if (const auto at = find_row(standing_row); at >= 0 && !_rows[at].bounds.is_empty())
		_groups_scroller._offset.y = _rows[at].bounds.top - standing_y;
	build_cells(focus_face);
}

bool faces_view::surname_collapsed(const std::string_view surname) const
{
	return (_filter.empty() ? _collapsed_surnames : _filtered_collapsed).contains(std::string(surname));
}

recti faces_view::expander_bounds(const int i) const
{
	if (_rows[i].what != row::kind::surname) return {};
	const auto b = _rows[i].bounds.offset(groups_offset());
	return {b.left, b.top, b.left + b.height(), b.bottom};
}

void faces_view::toggle_surname(const int i)
{
	if (i < 0 || i >= static_cast<int>(_rows.size()) || _rows[i].what != row::kind::surname) return;
	const auto& surname = _rows[i].surname;
	auto& collapsed = _filter.empty() ? _collapsed_surnames : _filtered_collapsed;
	if (!collapsed.erase(surname)) collapsed.insert(surname);
	_people_selection.move_to(_rows[i].key, true, false, {});
	_pane = pane::groups;
	layout_rows(nullptr);
	changed();
}

void faces_view::build_people_review(const std::vector<int>& rows)
{
	for (const auto i : rows) _people_review.push_back({i, _rows[i].detail, {}});
	_faces_title = tt.faces_people;
	_faces_detail = tt.faces_review_hint;
	_pane = pane::groups;
}

std::vector<std::string> faces_view::unknown_order::arrange(std::vector<std::vector<uint32_t>>& runs,
                                                            std::vector<uint32_t>& others,
                                                            std::vector<uint32_t>& unclear)
{
	constexpr auto unseen = std::numeric_limits<uint32_t>::max();
	const auto place_of = [this](const uint32_t f)
	{
		const auto it = place.find(f);
		return it == place.end() ? unseen : it->second;
	};

	if (!place.empty())
	{
		// Within a run and within a section, the faces shown before in the order they were shown, then any new to the
		// list in the overview's order.
		const auto settle = [&](std::vector<uint32_t>& faces)
		{
			std::vector<std::pair<uint32_t, uint32_t>> placed;
			placed.reserve(faces.size());
			for (const auto f : faces) placed.emplace_back(place_of(f), f);
			std::ranges::stable_sort(placed, {}, &std::pair<uint32_t, uint32_t>::first);
			for (size_t i = 0; i < faces.size(); ++i) faces[i] = placed[i].second;
		};
		for (auto& faces : runs) settle(faces);
		settle(others);
		settle(unclear);

		// Each run where its first face was: one re-formed from faces shown alone comes after the runs shown, and one
		// of faces new to the list after those, largest first.
		std::vector<std::pair<uint32_t, size_t>> placed;
		placed.reserve(runs.size());
		for (size_t i = 0; i < runs.size(); ++i) placed.emplace_back(place_of(runs[i].front()), i);
		std::ranges::stable_sort(placed, {}, &std::pair<uint32_t, size_t>::first);
		std::vector<std::vector<uint32_t>> ordered;
		ordered.reserve(runs.size());
		for (const auto& [where, i] : placed) ordered.push_back(std::move(runs[i]));
		runs = std::move(ordered);
	}

	const auto remember = [this](const std::vector<uint32_t>& faces)
	{
		for (const auto f : faces)
			if (place.try_emplace(f, next).second) ++next;
	};
	std::vector<std::string> keys;
	keys.reserve(runs.size());
	core::hash_set<uint32_t> used;
	for (const auto& faces : runs)
	{
		remember(faces);
		// The key of the run it was first shown as, even once that run's first face has left it, unless a run split
		// from the same one has it already.
		auto key = faces.front();
		if (const auto it = run.find(key); it != run.end() && !used.contains(it->second)) key = it->second;
		used.insert(key);
		for (const auto f : faces) run.try_emplace(f, key);
		keys.push_back(std::format("run:{}", key));
	}
	remember(others);
	remember(unclear);
	return keys;
}

void faces_view::build_cells(const std::optional<uint32_t> focus_face)
{
	if (!_pending_faces_anchor) _pending_faces_anchor = capture_faces_anchor();
	_hover = -1;
	_heading_hover = -1;
	_heading_button_hover = {-1, -1};
	_people_review.clear();
	_cells.clear();
	_cell_keys.clear();
	_headings.clear();
	_faces_title.clear();
	_faces_detail.clear();
	_review_person.clear();
	const auto rows = selected_rows();
	if (rows.size() > 1) build_people_review(rows);
	else if (rows.size() == 1)
	{
		const auto& r = _rows[rows.front()];
		_faces_title = r.title;
		if (r.named()) _review_person = r.title;
		const auto o = _model ? _model->overview : nullptr;
		const auto store = _model ? _model->store : nullptr;
		const auto add = [&](const faces_view_model::shown& s, const faces::member_kind kind, const float score)
		{
			auto& c = _cells.emplace_back();
			c.shown = s;
			c.caption = std::string(s.path.name());
			c.member = kind;
			c.score = score;
			c.hidden = r.what == row::kind::hidden_faces;
		};
		const auto face = [&](const uint32_t f, const faces::member_kind kind, const float score)
		{
			add(faces_view_model::shown::of(*store, f), kind, score);
		};
		if (r.named())
		{
			for (const auto i : r.overview_people)
				for (const auto& m : o->people[i].confirmed)
					if (!_moved.contains(m.face)) face(m.face, m.kind, m.score);
			for (const auto& [f, m] : _moved)
				if (m.row_key == r.key) face(f, m.kind, m.score);
			for (const auto i : r.file_people)
				for (const auto& s : _model->people[i].named)
					if (!s.face || (r.overview_people.empty() && !_moved.contains(*s.face)))
						add(s, faces::member_kind::named, faces::not_compared);
			const auto held = _cells.size();
			if (!_moved.empty() || r.overview_people.size() > 1)
				std::stable_sort(_cells.begin(), _cells.end(),
				                 [](const cell& a, const cell& b) { return a.score > b.score; });
			if (held) _headings.push_back({std::string(tt.faces_confirmed), {}, 0, {}, {}, "confirmed"});
			for (const auto i : r.overview_people)
				for (const auto& m : o->people[i].suggested)
					if (!_moved.contains(m.face)) face(m.face, m.kind, m.score);
			for (const auto& [f, m] : _moved)
				if (m.offer_key == r.key) face(f, faces::member_kind::suggested, m.score);
			if (_cells.size() != held)
			{
				// Confirm all changes the face book, so it is offered only where the book can be changed.
				std::vector<heading_button> buttons;
				if (book_editable()) buttons.push_back({heading_action::confirm_all});
				_headings.push_back({std::string(tt.faces_suggested), {}, held, std::move(buttons), {}, "suggested"});
				if (!_moved.empty() || r.overview_people.size() > 1)
					std::stable_sort(_cells.begin() + held, _cells.end(),
					                 [](const cell& a, const cell& b) { return a.score > b.score; });
			}
		}
		else if (o && store && r.what == row::kind::unknown)
		{
			// docs/faces.md#91-people: each run of lookalikes under a heading of its own, which selects them to be named
			// together, then the single faces, then the small or unclear ones. A face moved here since the overview was
			// made joins the single faces, and while Unknown stays selected what was shown keeps its order. Where the
			// face book can be changed, a run's heading can name or hide all of it, and the others' can hide theirs: a
			// person is in one run, never in a list of faces alike in nothing.
			const auto editable = book_editable();
			const auto buttons = [editable](const bool name)
			{
				std::vector<heading_button> result;
				if (editable && name) result.push_back({heading_action::name});
				if (editable) result.push_back({heading_action::hide});
				return result;
			};
			const auto& list = o->unknown_for_review();
			const auto unclear_from = o->unknown_by_similarity.empty() ? list.size() : o->unclear_from;
			std::vector<std::vector<uint32_t>> runs;
			size_t at = 0;
			for (const auto length : o->unknown_runs)
			{
				const auto run_end = std::min(list.size(), at + length);
				std::vector<uint32_t> run;
				for (auto i = at; i < run_end; ++i)
					if (!_moved.contains(list[i])) run.push_back(list[i]);
				if (!run.empty()) runs.push_back(std::move(run));
				at = run_end;
			}
			std::vector<uint32_t> others, unclear;
			for (auto i = at; i < std::min(unclear_from, list.size()); ++i)
				if (!_moved.contains(list[i])) others.push_back(list[i]);
			for (const auto& [f, m] : _moved)
				if (m.row_key == r.key) others.push_back(f);
			for (auto i = std::max(at, unclear_from); i < list.size(); ++i)
				if (!_moved.contains(list[i])) unclear.push_back(list[i]);

			const auto keys = _unknown_order.arrange(runs, others, unclear);
			for (size_t k = 0; k < runs.size(); ++k)
			{
				const auto heading = _cells.size();
				for (const auto f : runs[k]) face(f, faces::member_kind::unknown, 0);
				_headings.push_back({std::format("{} \u00b7 {}", tt.faces_similar,
				                                 platform::format_number(core::to_string(runs[k].size()))),
				                     std::string(tt.faces_similar),
				                     heading,
				                     buttons(true),
				                     {},
				                     keys[k]});
			}
			const auto others_at = _cells.size();
			for (const auto f : others) face(f, faces::member_kind::unknown, 0);
			if (_cells.size() > others_at)
				_headings.push_back({std::string(tt.faces_other), {}, others_at, buttons(false), {}, "other"});
			const auto unclear_at = _cells.size();
			for (const auto f : unclear) face(f, faces::member_kind::unknown, 0);
			if (_cells.size() > unclear_at)
				_headings.push_back({std::string(tt.faces_unclear), {}, unclear_at, buttons(false), {}, "unclear"});
		}
		else if (o && store)
		{
			for (const auto f : o->hidden)
				if (!_moved.contains(f)) face(f, faces::member_kind::unknown, 0);
			for (const auto& [f, m] : _moved)
				if (m.row_key == r.key) face(f, faces::member_kind::unknown, 0);
		}
		_faces_detail = count_faces(_cells.size());
	}
	for (const auto& c : _cells) _cell_keys.push_back(crop_key(c.shown));
	_face_selection.retain(_cell_keys);
	if (focus_face)
		for (size_t i = 0; i < _cells.size(); ++i)
			if (_cells[i].shown.face == focus_face)
			{
				_face_selection.click(_cell_keys[i], false, false, _cell_keys);
				_pane = pane::faces;
				_reveal_face = true;
				break;
			}
	layout_cells(nullptr);
	changed();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Layout
///////////////////////////////////////////////////////////////////////////////////////////////////

recti faces_view::groups_pane() const
{
	return {0, _title_height, groups_rail().left, std::max(_title_height, _extent.cy)};
}

recti faces_view::faces_pane() const
{
	return {_groups_width, _title_height, std::max(_groups_width, _extent.cx), std::max(_title_height, _extent.cy)};
}

recti faces_view::groups_rail() const
{
	return {std::max(0, _groups_width - _scroll_width), 0, _groups_width, _extent.cy};
}

recti faces_view::splitter_bounds() const
{
	auto rail = groups_rail();
	if (_groups_scroller.can_scroll()) rail.bottom = _groups_scroller.scroll_bounds().top;
	return rail;
}

int faces_view::clamp_groups_width(const int width) const
{
	const auto most = std::max(0, _extent.cx / 2);
	return std::clamp(width, std::min(most, core::round(160 * _scale_factor)), most);
}

pointi faces_view::groups_offset() const
{
	return pointi(0, _title_height) - _groups_scroller.scroll_offset();
}

pointi faces_view::faces_offset() const
{
	return pointi(_groups_width, _title_height) - _faces_scroller.scroll_offset();
}

recti faces_view::row_bounds(const int row) const
{
	if (row < 0 || row >= static_cast<int>(_rows.size())) return {};
	const auto b = _rows[row].bounds.offset(groups_offset()).intersection(groups_pane());
	return b.is_empty() ? recti{} : b;
}

recti faces_view::cell_bounds(const int cell) const
{
	if (cell < 0 || cell >= static_cast<int>(_cells.size())) return {};
	const auto b = _cells[cell].bounds.offset(faces_offset()).intersection(faces_pane());
	return b.is_empty() ? recti{} : b;
}

recti faces_view::heading_button_bounds(const heading_button& button) const
{
	if (button.bounds.is_empty()) return {};
	const auto b = button.bounds.offset(faces_offset()).intersection(faces_pane());
	return b.is_empty() ? recti{} : b;
}

faces_view::face_scroll_anchor faces_view::capture_faces_anchor() const
{
	face_scroll_anchor anchor;
	const auto scroll_y = _faces_scroller.scroll_offset().y;
	const auto height = _faces_scroller.client_bounds().height();
	anchor.offset = scroll_y;
	anchor.range = std::max(0, _faces_scroller._scroll_extent.cy - height);
	if (height <= 0) return anchor;

	// docs/faces.md#92-faces: Unknown and Hidden only lose faces while they are shown, so the page closes up below its
	// top rather than moving what is above the faces that left.
	if (const auto rows = selected_rows(); rows.size() == 1 && (_rows[rows.front()].what == row::kind::unknown ||
	                                                            _rows[rows.front()].what == row::kind::hidden_faces))
	{
		const auto bottom = scroll_y + height;
		const auto first = static_cast<size_t>(std::ranges::upper_bound(_cells, scroll_y, {}, [](const cell& c)
		{ return c.bounds.bottom; }) - _cells.begin());
		auto h = static_cast<size_t>(std::ranges::lower_bound(_headings, first, {}, &heading::before) -
		                             _headings.begin());
		for (auto c = first;; ++c)
		{
			for (; h < _headings.size() && _headings[h].before <= c; ++h)
				if (const auto& b = _headings[h].bounds; !b.is_empty() && b.bottom > scroll_y && b.top < bottom)
					anchor.in_view.push_back({_headings[h].key, b.top - scroll_y, true});
			if (c >= _cells.size() || _cells[c].bounds.top >= bottom) break;
			anchor.in_view.push_back({_cell_keys[c], _cells[c].bounds.top - scroll_y, false});
		}
		return anchor;
	}

	// A visible heading stays put even when confirming a face adds rows above it.
	for (const auto& h : _headings)
		if (!h.bounds.is_empty() && h.bounds.bottom > scroll_y && h.bounds.top < scroll_y + height)
			anchor.heading = {h.key, h.bounds.top - scroll_y};

	const auto mid_y = scroll_y + height / 2;
	const auto focus = faces_targeted() ? focus_cell() : -1;
	if (scroll_y == 0 && !anchor.heading && cell_bounds(focus).is_empty()) return anchor;
	const auto first = std::ranges::upper_bound(_cells, scroll_y, {}, [](const cell& c) { return c.bounds.bottom; });
	std::vector<size_t> visible;
	for (auto at = first; at != _cells.end() && at->bounds.top < scroll_y + height; ++at)
		visible.push_back(static_cast<size_t>(at - _cells.begin()));
	std::ranges::stable_sort(visible, {}, [&](const size_t i)
	{ return static_cast<int>(i) == focus ? -1 : std::abs(_cells[i].bounds.center().y - mid_y); });
	for (const auto i : visible)
		anchor.portraits.push_back(
			{_cell_keys[i], _cells[i].bounds.top - scroll_y, _cells[i].member == faces::member_kind::suggested});
	return anchor;
}

int faces_view::restore_faces_anchor(const face_scroll_anchor& anchor, const int height) const
{
	if (!anchor.in_view.empty())
	{
		const auto first_y = anchor.in_view.front().y;
		for (const auto& shown : anchor.in_view)
		{
			int top = 0;
			if (shown.heading)
			{
				const auto at = std::ranges::find(_headings, shown.key, &heading::key);
				if (at == _headings.end()) continue;
				top = at->bounds.top;
			}
			else
			{
				const auto at = std::ranges::find(_cell_keys, shown.key);
				if (at == _cell_keys.end()) continue;
				top = _cells[at - _cell_keys.begin()].bounds.top;
			}
			// Where it was; or, where what was above it in view has left, up in the place of the first - though no
			// higher than the top of the page, so it is not left half hidden.
			return top - std::min(shown.y, std::max(first_y, 0));
		}
	}
	if (anchor.heading)
	{
		const auto& [key, y] = *anchor.heading;
		const auto at = std::ranges::find(_headings, key, &heading::key);
		if (at != _headings.end()) return at->bounds.top - y;
	}
	for (const auto& p : anchor.portraits)
	{
		const auto at = std::ranges::find(_cell_keys, p.key);
		if (at == _cell_keys.end()) continue;
		const auto& c = _cells[at - _cell_keys.begin()];
		// A confirmed suggestion is now review history, not where the user was working.
		if ((c.member == faces::member_kind::suggested) == p.suggested) return c.bounds.top - p.y;
	}
	const auto range = std::max(0, _faces_content_height - height);
	return anchor.range > 0 ? core::mul_div(anchor.offset, range, anchor.range) : anchor.offset;
}

void faces_view::layout(render::measure_context& mc, const sizei extent)
{
	auto saved_anchor = std::exchange(_pending_faces_anchor, {});
	const auto anchor = saved_anchor ? std::move(*saved_anchor) : capture_faces_anchor();

	_extent = extent;
	_scale_factor = mc.scale_factor;
	_line_height = mc.text_line_height(render::font_face::dialog);
	_scroll_width = mc.scroll_width;
	const auto title_line = mc.text_line_height(render::font_face::title);
	_title_height = title_line + _line_height + mc.padding2 * 2;
	_row_height = std::max(_line_height * 2 + core::round(10 * _scale_factor), core::round(48 * _scale_factor));

	const auto preferred = setting.faces.groups_width > 0 ? core::round(setting.faces.groups_width * _scale_factor)
	                                                      : core::round(208 * _scale_factor);
	_groups_width = clamp_groups_width(_drag_groups_width.value_or(preferred));

	layout_rows(&mc);

	// While the faces pane scrolls, its scroll bar names its sections as Items' does, as wide as their widest word: a
	// name wraps where its band has the height. A narrower grid only grows taller, so a pane that scrolls with a plain
	// bar still scrolls with the wider one.
	_faces_rail_width = _scroll_width;
	layout_cells(&mc);
	if (_faces_content_height > extent.cy - _title_height)
	{
		if (const auto rail = faces_rail_width(mc); rail > _faces_rail_width)
		{
			_faces_rail_width = rail;
			layout_cells(&mc);
		}
	}

	// Like the embedded sidebar: one column, with the thumb in the lower half only when the list overflows.
	const auto client = groups_pane();
	auto rail = groups_rail();
	rail.top = rail.center().y;
	_groups_scroller.layout({client.width(), _groups_content_height}, client, rail);

	const auto left = _groups_width;
	const auto faces_scroll_left = std::max(left, extent.cx - _faces_rail_width);
	const recti faces_client{left, _title_height, faces_scroll_left, extent.cy};
	_faces_scroller._offset.y = restore_faces_anchor(anchor, faces_client.height());
	_faces_scroller.layout({faces_client.width(), _faces_content_height}, faces_client,
	                       recti{faces_scroll_left, _title_height, extent.cx, extent.cy});
	_faces_scroller.sections_from_runs(faces_runs(), _faces_content_height);
	if (std::exchange(_reveal_face, false)) ensure_visible(focus_cell());
	update_selection_marks();

	// The faces pane's title renames what it shows, where that has a name to change.
	_faces_title_bounds = {};
	const auto rows = selected_rows();
	if (rows.size() == 1 && can_rename_row(rows.front()))
	{
		const auto pad = core::round(12 * _scale_factor);
		const auto text = mc.measure_text(_faces_title, render::font_face::title, render::text_style::single_line,
		                                  std::max(1, extent.cx - left - pad * 2));
		const auto top = mc.padding2;
		_faces_title_bounds = recti(left + pad, top, left + pad + text.cx + mc.icon_cxy + mc.padding2, top + text.cy)
		                          .intersection(recti(left, top, extent.cx - pad, top + title_line));
	}

	if (_active) request_crops();
}

void faces_view::groups_width(const int width)
{
	const auto next = clamp_groups_width(width);
	if (next == _drag_groups_width.value_or(_groups_width)) return;
	_drag_groups_width = next;
	_state.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw);
}

void faces_view::finish_groups_resize(const bool keep)
{
	auto invalid = view_invalid::view_redraw;
	if (const auto width = std::exchange(_drag_groups_width, {}))
	{
		invalid |= view_invalid::view_layout;
		if (keep)
		{
			setting.faces.groups_width = core::round(clamp_groups_width(*width) / _scale_factor);
			invalid |= view_invalid::options_save;
		}
	}
	_state.invalidate_view(invalid);
}

// Inboxes always have a row. Each visible family heading is followed by its expanded entries.
void faces_view::layout_rows(render::measure_context* mc)
{
	const auto pad = core::round(8 * _scale_factor);
	const auto gap = core::round(2 * _scale_factor);
	const auto indent = core::round(10 * _scale_factor);
	const auto header = _line_height + core::round(12 * _scale_factor);
	const auto width = groups_pane().width();
	const auto thumb = std::max(1, _row_height - core::round(8 * _scale_factor));
	const auto text_gap = core::round(8 * _scale_factor);
	auto y = pad;

	_no_matches_bounds = {};
	_clear_filter_link->is_visible(!_filter.empty());
	if (_model && !_filter.empty() && std::ranges::none_of(_rows, [](const row& r) { return r.named(); }))
	{
		const auto inner = std::max(1, width - pad * 2);
		const auto height = mc ? mc->measure_text(tt.faces_no_matches, render::font_face::dialog,
		                                          render::text_style::multiline, inner)
		                             .cy
		                       : _line_height;
		_no_matches_bounds = {pad, y, pad + inner, y + height};
		y = _no_matches_bounds.bottom + gap;
	}
	if (_clear_filter_link->is_visible())
	{
		const auto inner = std::max(1, width - pad * 2);
		auto size = sizei{inner, _line_height};
		if (mc)
		{
			_clear_filter_link->text(tt.faces_clear_filter);
			size = _clear_filter_link->measure(*mc, inner);
		}
		_clear_filter_link->bounds = {pad, y, pad + size.cx, y + size.cy};
		y += size.cy + pad;
	}

	for (size_t i = 0; i < _rows.size(); ++i)
	{
		auto& r = _rows[i];
		const auto heading = r.what == row::kind::surname;
		if (r.named() && surname_collapsed(r.surname))
		{
			r.bounds = r.title_bounds = {};
			continue;
		}
		if (heading) y += pad / 2;
		r.bounds = {pad + (r.named() && !r.surname.empty() ? indent : 0), y, std::max(pad + indent, width - pad),
		            y + (heading ? header : _row_height)};
		y = r.bounds.bottom + gap;
		const auto portraits = std::max(1, static_cast<int>(r.previews.size()));
		const auto text_left = r.bounds.left + (heading ? header
		                                                : core::round(4 * _scale_factor) + thumb +
		                                                      (portraits - 1) * thumb / 2 + text_gap);
		auto text_width = std::max(0, r.bounds.right - text_left);
		r.title_clipped = r.detail_clipped = false;
		if (mc)
		{
			constexpr auto unbounded = std::numeric_limits<int>::max() / 4;
			const auto measure = [mc](const std::string_view text)
			{
				return mc->measure_text(text, render::font_face::dialog, render::text_style::single_line, unbounded).cx;
			};
			// A person's lines are drawn short of the row's edge by its inset; a heading's to the edge.
			const auto room = heading ? text_width : std::max(0, text_width - core::round(4 * _scale_factor));
			const auto title_width = measure(r.title);
			r.title_clipped = title_width > room;
			r.detail_clipped = !heading && measure(r.detail) > room;
			text_width = std::min(text_width, title_width);
		}
		const auto text_top = heading ? r.bounds.top + (header - _line_height) / 2
		                              : r.bounds.top + std::max(0, r.bounds.height() / 2 - _line_height);
		r.title_bounds = recti(text_left, text_top, text_left + text_width, text_top + _line_height);
	}

	_groups_content_height = y + pad;
}

// Faces in rows, each a square with its caption and detail under it; a heading takes a row of its own, wrapped to
// the width its action's link leaves it, before the cells added after it. With face search off, what it does and the
// button that turns it on lead.
void faces_view::layout_cells(render::measure_context* mc)
{
	const auto pad = core::round(12 * _scale_factor);
	const auto gap = core::round(12 * _scale_factor);
	const auto width = std::max(0, faces_pane().width() - _faces_rail_width);
	_turn_on_text = {};
	_turn_on_bounds = {};

	if (reviewing_rows())
	{
		auto y = pad;
		const auto right = std::max(pad, width - pad);
		for (auto& preview : _people_review)
		{
			preview.bounds = {pad, y, right, y + _row_height};
			y += _row_height + gap;
		}
		_faces_content_height = y;
		return;
	}

	const auto caption = _line_height * 2 + core::round(6 * _scale_factor);
	const auto usable = std::max(1, width - pad * 2);
	const auto size = std::clamp(setting.faces.tile_size, settings_t::faces_t::min_tile_size,
	                             settings_t::faces_t::max_tile_size);
	_tile = std::clamp(core::round(size * _scale_factor), 1, usable);
	const auto per_row = std::max(1, (usable + gap) / (_tile + gap));
	const auto row_height = _tile + caption + gap;

	auto y = pad;
	auto row_start = size_t{0};
	auto col = 0;

	if (!_state.item_index.faces().is_enabled())
	{
		auto height = _line_height * 3;
		if (mc)
		{
			height = mc->measure_text(tt.faces_search_off, render::font_face::dialog, render::text_style::multiline,
			                          usable)
			             .cy;
		}
		_turn_on_text = recti(pad, y, pad + usable, y + height);
		y = _turn_on_text.bottom + gap;
		_turn_on_bounds = recti(pad, y, pad + std::min(usable, _tile * 3), y + _line_height * 2);
		y = _turn_on_bounds.bottom + gap * 2;
	}

	const auto finish_row = [&]
	{
		if (col == 0) return;
		const auto row_width = col * (_tile + gap) - gap;
		const auto left = (usable - row_width) / 2;
		for (auto i = row_start; i < row_start + col; ++i) _cells[i].bounds = _cells[i].bounds.offset(pointi(left, 0));
		y += row_height;
		col = 0;
	};

	size_t h = 0;
	const auto headings_before = [&](const size_t index)
	{
		while (h < _headings.size() && _headings[h].before <= index)
		{
			finish_row();

			auto& at = _headings[h];
			const auto inset = core::round(8 * _scale_factor);
			const auto right = pad + usable - inset;
			const auto spacing = core::round(4 * _scale_factor);

			// The buttons end the heading's line, in the order they read; its text wraps in what they leave.
			std::vector<std::pair<int, int>> spans(at.buttons.size());
			auto buttons_left = right;
			if (mc)
			{
				const auto limit = std::max(1, usable - inset * 2);
				for (auto i = at.buttons.size(); i-- > 0;)
				{
					const auto width = mc->icon_cxy + mc->padding1 * 3 +
					                   mc->measure_text(heading_action_text(at.buttons[i].action),
					                                    render::font_face::dialog, render::text_style::single_line,
					                                    limit)
					                       .cx;
					const auto left = std::max(pad + inset, buttons_left - width);
					spans[i] = {left, buttons_left};
					buttons_left = left - spacing;
				}
			}

			const auto has_buttons = mc && !at.buttons.empty();
			const auto text_right = has_buttons ? std::max(pad + inset + 1, buttons_left - inset) : right;
			auto height = _line_height;
			if (mc)
			{
				height = std::max(height,
				                  mc->measure_text(at.text, render::font_face::dialog, render::text_style::multiline,
				                                   std::max(1, text_right - pad - inset))
				                      .cy);
			}
			at.bounds = recti(pad, y, pad + usable, y + height + gap);
			// Each button is a line tall, centred on the heading.
			const auto button_height = std::min(at.bounds.height(), _line_height + core::round(6 * _scale_factor));
			const auto button_top = at.bounds.top + (at.bounds.height() - button_height) / 2;
			for (size_t i = 0; i < at.buttons.size(); ++i)
			{
				const auto [left, button_right] = spans[i];
				at.buttons[i].bounds = has_buttons && button_right > left
				                           ? recti(left, button_top, button_right, button_top + button_height)
				                           : recti{};
			}
			y = at.bounds.bottom + gap;
			++h;
		}
	};

	for (size_t i = 0; i < _cells.size(); ++i)
	{
		headings_before(i);
		if (col == 0) row_start = i;
		const auto x = pad + col * (_tile + gap);
		_cells[i].bounds = recti(x, y, x + _tile, y + _tile + caption);
		if (++col == per_row) finish_row();
	}

	headings_before(_cells.size());
	finish_row();
	_faces_content_height = y + pad;
}

std::vector<ui::scroller_run> faces_view::faces_runs() const
{
	std::vector<ui::scroller_run> runs;
	if (_headings.empty()) return runs;

	// The faces before the first heading are the group's own, named as the pane is.
	if (_headings.front().before > 0) runs.push_back({_faces_title, {}, 0});
	for (const auto& h : _headings) runs.push_back({h.label.empty() ? h.text : h.label, {}, h.bounds.top});
	return runs;
}

// Fit the widest word of the section names, which wrap where their bands have the height, reserving most of the pane
// for the face grid.
int faces_view::faces_rail_width(render::measure_context& mc) const
{
	const auto runs = faces_runs();
	if (runs.empty()) return _scroll_width;

	const auto pane = std::max(1, faces_pane().width());
	const auto figures = mc.measure_text("88888", render::font_face::dialog, render::text_style::single_line, pane).cx;
	auto widest = 0;
	for (const auto& r : runs) widest = std::max(widest, ui::table_layout::widest_word(mc, r.text));

	const auto most = std::max(_scroll_width, pane / 4);
	const auto least = std::min(most, std::max(_scroll_width, figures));
	return std::clamp(widest + mc.padding1 * 2, least, most);
}

void faces_view::update_selection_marks()
{
	std::vector<int> groups;
	if (!_people_selection.empty())
	{
		// A folded selection stays marked at its surname heading.
		int centre = 0;
		for (const auto& r : _rows)
		{
			if (!r.bounds.is_empty()) centre = r.bounds.center().y;
			else if (!r.named()) continue;
			if (_people_selection.is_selected(r.key) && (groups.empty() || groups.back() != centre))
				groups.push_back(centre);
		}
	}
	_groups_scroller.marks(std::move(groups), ui::style::colors.view_selected_background);

	std::vector<int> centres;
	if (!_face_selection.empty())
	{
		for (size_t i = 0; i < _cells.size(); ++i)
		{
			if (!_face_selection.is_selected(_cell_keys[i])) continue;
			const auto y = _cells[i].bounds.center().y;
			if (centres.empty() || centres.back() != y) centres.push_back(y);
		}
	}
	_faces_scroller.marks(std::move(centres), ui::style::colors.view_selected_background);
}

void faces_view::add_crop_preview(view_hover_element& hover, const faces_view_model::shown& face)
{
	if (face.path.is_empty()) return;
	const auto key = crop_key(face);
	if (const auto crop = _crops.find(key); crop != _crops.end() && crop->second.surface)
	{
		hover.elements->add(
			std::make_shared<surface_element>(crop->second.surface, _tile, flex_item::center | flex_item::new_line));
	}
	else if (_preview_face.path.is_empty() || crop_key(_preview_face) != key)
	{
		_preview_face = face;
		request_crops();
	}
}

// As Items' scroll bar does: the face at the point of the list the pointer is over on the bar, and the name of the
// section it is in. A face far from what shows is cut for the bubble, which shows it once it is.
void faces_view::faces_scroll_popup(view_hover_element& hover, const pointi loc)
{
	if (_cells.empty()) return;

	// The rows run down the pane, so the first face whose row reaches the point is the one there.
	const auto y = _faces_scroller.scrollbar_pos_to_logical(loc.y);
	const auto found = std::ranges::lower_bound(_cells, y, {}, [](const cell& c) { return c.bounds.bottom; });
	const auto index = std::min(static_cast<size_t>(found - _cells.begin()), _cells.size() - 1);
	const auto& c = _cells[index];

	std::string_view section;
	if (!_headings.empty())
	{
		section = _faces_title;
		for (const auto& h : _headings)
		{
			if (h.before > index) break;
			section = h.text;
		}
	}

	add_crop_preview(hover, c.shown);
	hover.elements->add(std::make_shared<text_element>(section.empty() ? std::string_view(c.caption) : section,
	                                                   render::font_face::dialog, render::text_style::multiline,
	                                                   flex_item::center | flex_item::new_line));

	const auto bar = _faces_scroller.scroll_bounds();
	hover.window_bounds = recti(bar.left, loc.y, bar.right, loc.y + _line_height);
	hover.active_bounds = recti(bar.left, loc.y - 1, bar.right, loc.y + 1);
	hover.active_delay = bubble_delay::scrollbar_entry;
	hover.horizontal = true;
	hover.preferred_size = 200;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Drawing
///////////////////////////////////////////////////////////////////////////////////////////////////

void faces_view::draw_crop(render::draw_context& dc, const faces_view_model::shown& s, const recti square,
                           const float alpha, const int radius) const
{
	const auto found = _crops.find(crop_key(s));

	if (found != _crops.end() && found->second.surface)
	{
		const auto& crop = found->second;
		if (!crop.texture)
		{
			crop.texture = dc.create_texture();
			if (crop.texture && crop.texture->update(crop.surface) == render::texture_update_result::failed)
				crop.texture.reset();
		}

		if (crop.texture)
		{
			const auto dims = crop.surface->dimensions();
			dc.draw_texture(crop.texture, square, recti(0, 0, dims.cx, dims.cy), alpha,
			                render::texture_sampler::bilinear, static_cast<float>(radius));
			return;
		}
	}

	dc.draw_rounded_rect(square, core::color(ui::style::colors.view_text, alpha * 0.08f), radius);
	xdraw_icon(dc, icon_index::person, square, core::color(ui::style::colors.view_text, alpha * 0.4f), {});
}

void faces_view::render(render::draw_context& dc, view_controller_ptr controller)
{
	const auto alpha = dc.colors.alpha;
	const auto text_clr = core::color(ui::style::colors.view_text, alpha);
	const auto dim_clr = core::color(ui::style::colors.view_text, alpha * 0.6f);
	const auto pad = core::round(12 * _scale_factor);

	// The People pane, in the sidebar's colour: its title, then its rows.
	dc.draw_rect(recti(0, 0, _groups_width, _extent.cy), core::color(ui::style::colors.sidebar_background, alpha));
	const auto sub_top = _title_height - dc.padding2 - _line_height;
	const auto title_right = std::max(pad, groups_pane().right - pad);
	dc.draw_text(tt.faces_people, recti(pad, dc.padding2, title_right, sub_top), render::font_face::title,
	             render::text_style::single_line, text_clr, {});
	dc.draw_text(tt.collection_title, recti(pad, sub_top, title_right, sub_top + _line_height),
	             render::font_face::dialog, render::text_style::single_line, dim_clr, {});

	{
		render::scoped_clip clip(dc, groups_pane());
		draw_rows(dc);
	}

	_groups_scroller.draw_scroll(dc);
	const auto* splitter = dynamic_cast<const faces_splitter_controller*>(controller.get());
	const auto handle = splitter_bounds();
	draw_splitter_handle(dc, handle, handle.width(), splitter != nullptr, splitter && splitter->tracking());

	// The center pane: one page of faces, or a preview of the selected pages.
	const auto left = _groups_width;
	const recti title_bounds{left + pad, dc.padding2, std::max(left + pad, _extent.cx - pad), sub_top};

	if (!_faces_detail.empty())
	{
		dc.draw_text(_faces_detail, recti(title_bounds.left, sub_top, title_bounds.right, sub_top + _line_height),
		             render::font_face::dialog, render::text_style::single_line, dim_clr, {});
	}

	if (!_faces_title_bounds.is_empty())
	{
		const auto link_clr = core::color(link_foreground_color(), alpha);
		auto name = _faces_title_bounds;
		name.right = std::max(name.left, name.right - dc.icon_cxy - dc.padding2);
		dc.draw_text(_faces_title, name, render::font_face::title, render::text_style::single_line, link_clr, {});
		const recti pencil(_faces_title_bounds.right - dc.icon_cxy, _faces_title_bounds.top, _faces_title_bounds.right,
		                   _faces_title_bounds.bottom);
		xdraw_icon(dc, icon_index::rename, pencil, link_clr, {});
	}
	else
	{
		dc.draw_text(_faces_title.empty() ? tt.command_faces.sv() : std::string_view(_faces_title), title_bounds,
		             render::font_face::title, render::text_style::single_line, text_clr, {});
	}

	{
		render::scoped_clip clip(dc, faces_pane());
		if (reviewing_rows()) draw_people_review(dc);
		else draw_cells(dc);

		// The rubber band, as every list draws its own.
		if (_marquee.active)
		{
			const auto band = _marquee.bounds().offset(faces_offset()).intersection(faces_pane());
			if (!band.is_empty()) dc.draw_rect(band, ui::marquee_fill(alpha));
		}
	}

	_faces_scroller.draw_scroll(dc);
	draw_drag(dc);
}

void faces_view::draw_people_review(render::draw_context& dc) const
{
	const auto offset = faces_offset();
	const auto clip = faces_pane();
	const auto alpha = dc.colors.alpha;
	const auto text = core::color(ui::style::colors.view_text, alpha);
	const auto detail = text.aa(alpha * 0.7f);
	const auto pad = dc.padding2;
	const auto first = std::ranges::lower_bound(_people_review, clip.top - offset.y, {},
	                                            [](const person_preview& p) { return p.bounds.bottom; });

	for (auto p = first; p != _people_review.end(); ++p)
	{
		const auto bounds = p->bounds.offset(offset);
		if (bounds.top >= clip.bottom) break;
		const auto& row = _rows[p->row];
		dc.draw_rounded_rect(bounds, text.aa(alpha * 0.06f), dc.padding1);
		const auto side = std::max(0, std::min(bounds.height(), bounds.width()) - 2 * pad);
		const recti crop(bounds.left + pad, bounds.top + pad, bounds.left + pad + side, bounds.top + pad + side);
		if (!crop.is_empty()) draw_crop(dc, row.face, crop, alpha, dc.padding1);
		const auto x = crop.right + pad;
		if (x >= bounds.right - pad) continue;
		const auto y = bounds.center().y;
		dc.draw_text(row.title, {x, y - _line_height, bounds.right - pad, y}, render::font_face::dialog,
		             render::text_style::single_line, text, {});
		dc.draw_text(p->detail, {x, y, bounds.right - pad, y + _line_height}, render::font_face::dialog,
		             render::text_style::single_line, detail, {});
	}
}

void faces_view::draw_rows(render::draw_context& dc) const
{
	const auto offset = groups_offset();
	const auto clip = groups_pane();
	const auto alpha = dc.colors.alpha;
	const auto dim_clr = core::color(ui::style::colors.view_text, alpha * 0.6f);
	// The selection the commands act on is drawn as every list's selection is, and the other pane's muted, as a
	// selection is in a pane without the keyboard.
	const auto groups_active = !faces_targeted() && dc.frame_has_focus;
	const auto selected_clr = [groups_active, alpha](const bool hovered)
	{
		return ui::selection_fill(groups_active, hovered).aa(alpha);
	};
	const auto hover_clr = ui::hover_fill().aa(alpha);
	const auto target_clr = core::color(link_foreground_color(), alpha);
	const auto radius = core::round(6 * _scale_factor);
	const auto inset = core::round(4 * _scale_factor);
	const auto thumb = std::max(1, _row_height - inset * 2);
	// A name or a count the pane is too narrow for ends in an ellipsis, as a cell too narrow for its text does.
	const auto fit = [&dc](const std::string& text, const bool clipped, const recti bounds)
	{
		return clipped ? ui::table_layout::fit_text(dc, text, bounds.width()) : text;
	};
	for (size_t i = 0; i < _rows.size(); ++i)
	{
		const auto& r = _rows[i];
		if (r.bounds.is_empty()) continue;
		const auto b = r.bounds.offset(offset);
		if (!b.intersects(clip)) continue;

		const auto is_target = _drag.active && _drag.target.valid && _drag.target.row == static_cast<int>(i);
		const auto text_clr = core::color(ui::style::colors.view_text, alpha);
		const auto title = r.title_bounds.offset(offset);

		const auto hovered = static_cast<int>(i) == _row_hover && !_drag.active;
		if (is_row_selected(static_cast<int>(i))) dc.draw_rounded_rect(b, selected_clr(hovered), radius);
		else if (hovered) dc.draw_rounded_rect(b, hover_clr, radius);

		if (r.what == row::kind::surname)
		{
			xdraw_icon(dc, surname_collapsed(r.title) ? icon_index::right : icon_index::down,
			           expander_bounds(static_cast<int>(i)), text_clr, {});
			dc.draw_text(fit(r.title, r.title_clipped, title), title, render::font_face::dialog,
			             render::text_style::single_line, text_clr, {});
		}
		else
		{
			recti square(b.left + inset, b.top + inset, b.left + inset + thumb, b.top + inset + thumb);
			if (r.previews.empty()) draw_crop(dc, r.face, square, alpha, radius);
			for (const auto& sample : r.previews)
			{
				draw_crop(dc, sample, square, alpha, radius);
				square = square.offset(thumb / 2, 0);
			}

			const recti title_line(title.left, b.top + std::max(0, b.height() / 2 - _line_height), b.right - inset,
			                       b.top + b.height() / 2);
			const recti detail_line(title.left, b.top + b.height() / 2, b.right - inset,
			                        b.top + b.height() / 2 + _line_height);
			dc.draw_text(fit(r.title, r.title_clipped, title_line), title_line, render::font_face::dialog,
			             render::text_style::single_line, text_clr, {});
			dc.draw_text(fit(r.detail, r.detail_clipped, detail_line), detail_line, render::font_face::dialog,
			             render::text_style::single_line, dim_clr, {});
		}

		if (is_target)
		{
			const auto ring = core::round(2 * _scale_factor);
			dc.draw_border(b, b.inflate(ring), target_clr, target_clr);
		}

		if (_keyboard_used && _view_has_focus && _pane == pane::groups && _people_selection.cursor() == r.key)
			draw_keyboard_ring(dc, b);
	}

	if (!_no_matches_bounds.is_empty())
	{
		dc.draw_text(tt.faces_no_matches, _no_matches_bounds.offset(offset), render::font_face::dialog,
		             render::text_style::multiline, dim_clr, {});
	}
	else if (_rows.empty() && _model)
	{
		ui::draw_empty_message(dc, clip.inflate(-core::round(12 * _scale_factor)),
		                       _state.item_index.faces().is_enabled() ? tt.faces_none : tt.face_search_off);
	}
	if (_clear_filter_link->is_visible()) _clear_filter_link->render(dc, offset);
}

std::vector<keyboard_region> faces_view::keyboard_regions()
{
	if (!_clear_filter_link->is_visible()) return {};
	return {{keyboard_area::listing,
	         {_clear_filter_link},
	         groups_offset(),
	         groups_pane(),
	         [this](const recti r) { scroll_into_view(_groups_scroller, _host, r); }}};
}

void faces_view::draw_cells(render::draw_context& dc) const
{
	const auto offset = faces_offset();
	const auto clip = faces_pane();
	const auto alpha = dc.colors.alpha;
	const auto text_clr = core::color(ui::style::colors.view_text, alpha);
	const auto dim_clr = core::color(ui::style::colors.view_text, alpha * 0.6f);
	// The selection the commands act on - or the one a rubber band is making - is drawn as every list's selection
	// is, and the other pane's muted.
	const auto faces_active = (faces_targeted() || _marquee.active) && dc.frame_has_focus;
	const auto selected_clr = [faces_active, alpha](const bool hovered)
	{
		return ui::selection_fill(faces_active, hovered).aa(alpha);
	};
	const auto hover_clr = ui::hover_fill().aa(alpha);
	const auto radius = core::round(6 * _scale_factor);
	const auto pad = core::round(4 * _scale_factor);
	const auto band_has_cells = _marquee.active && !cells_in(_marquee.bounds()).empty();
	const auto focus = _keyboard_used && _view_has_focus && _pane == pane::faces ? focus_cell() : -1;

	if (!_turn_on_text.is_empty())
	{
		dc.draw_text(tt.faces_search_off, _turn_on_text.offset(offset), render::font_face::dialog,
		             render::text_style::multiline, text_clr, {});
		const auto button = _turn_on_bounds.offset(offset);
		dc.draw_rounded_rect(button, core::color(ui::style::colors.view_selected_background, alpha), radius);
		dc.draw_text(tt.face_scan, button, render::font_face::dialog, render::text_style::single_line_center, text_clr,
		             {});
	}

	for (size_t h = 0; h < _headings.size(); ++h)
	{
		const auto& at = _headings[h];
		const auto b = at.bounds.offset(offset);
		if (!b.intersects(clip)) continue;
		// A heading selects the faces under it, so the pointer over it shows it can be clicked.
		const auto hovered = static_cast<int>(h) == _heading_hover && !_marquee.active;
		const auto inset = core::round(8 * _scale_factor);
		auto text_bounds = b.inflate(-inset, 0);
		dc.draw_rounded_rect(b, core::color(ui::style::colors.view_text, alpha * (hovered ? 0.14f : 0.06f)), radius);

		// Its buttons end its line, each acting on every face under it.
		for (size_t i = 0; i < at.buttons.size(); ++i)
		{
			const auto& button = at.buttons[i];
			if (button.bounds.is_empty()) continue;
			const auto bounds = button.bounds.offset(offset);
			const auto pointed_at = _heading_button_hover == std::pair{static_cast<int>(h), static_cast<int>(i)};
			dc.draw_rounded_rect(bounds, core::color(ui::style::colors.view_text, alpha * (pointed_at ? 0.24f : 0.1f)),
			                     radius);
			const recti icon(bounds.left + dc.padding1, bounds.top, bounds.left + dc.padding1 + dc.icon_cxy,
			                 bounds.bottom);
			xdraw_icon(dc, heading_action_icon(button.action), icon, text_clr, {});
			dc.draw_text(heading_action_text(button.action),
			             recti(icon.right + dc.padding1, bounds.top, bounds.right, bounds.bottom),
			             render::font_face::dialog, render::text_style::single_line, text_clr, {});
			text_bounds.right = std::min(text_bounds.right, std::max(text_bounds.left, bounds.left - inset));
		}

		dc.draw_text(at.text, text_bounds, render::font_face::dialog, render::text_style::multiline,
		             hovered ? core::color(link_foreground_color(), alpha) : text_clr, {});
	}

	if (!_model)
	{
		ui::draw_empty_message(dc, clip.inflate(-pad * 4), tt.faces_reading);
	}
	else if (_cells.empty() && _state.item_index.faces().is_enabled() && !_model->overview)
	{
		ui::draw_empty_message(dc, clip.inflate(-pad * 4), tt.faces_finding);
	}

	const auto first = std::ranges::lower_bound(_cells, clip.top - offset.y, {},
	                                            [](const cell& c) { return c.bounds.bottom; });
	for (auto it = first; it != _cells.end() && it->bounds.top <= clip.bottom - offset.y; ++it)
	{
		const auto i = static_cast<int>(it - _cells.begin());
		const auto& c = *it;
		const auto b = c.bounds.offset(offset);
		if (!b.intersects(clip)) continue;

		const auto is_selected = shown_selected(static_cast<int>(i), band_has_cells);
		const auto is_dragged = _drag.active && _drag.from == pane::faces &&
		                        std::ranges::find(_drag.items, static_cast<int>(i)) != _drag.items.end();

		const auto hovered = static_cast<int>(i) == _hover && !_drag.active && !_marquee.active;
		if (is_selected) dc.draw_rounded_rect(b.inflate(pad), selected_clr(hovered), radius);
		else if (hovered) dc.draw_rounded_rect(b.inflate(pad), hover_clr, radius);

		const recti square{b.left, b.top, b.right, b.top + _tile};
		const auto faded = is_dragged ? 0.4f : c.hidden ? 0.6f : 1.0f;
		draw_crop(dc, c.shown, square, alpha * faded, radius);

		if (hovered && c.member == faces::member_kind::suggested && book_editable())
		{
			for (const auto action : {suggestion_action::confirm, suggestion_action::reject, suggestion_action::name})
			{
				const auto button = suggestion_button_bounds(static_cast<int>(i), action);
				dc.draw_rounded_rect(button, core::color(ui::style::colors.bubble_background, alpha), radius);
				const auto glyph = action == suggestion_action::confirm  ? icon_index::check
				                   : action == suggestion_action::reject ? icon_index::remove2
				                                                         : icon_index::rename;
				xdraw_icon(dc, glyph, button, text_clr, {});
			}
		}

		if (is_selected && (_pane == pane::faces || _marquee.active) && !_drag.active)
		{
			const auto edge = dc.icon_cxy + pad;
			const recti check(square.right - pad - edge, square.top + pad, square.right - pad, square.top + pad + edge);
			dc.draw_rounded_rect(check, core::color(ui::style::colors.view_selected_background, alpha), edge / 2);
			xdraw_icon(dc, icon_index::check, check, text_clr, {});
		}

		const recti caption{b.left - pad, square.bottom + pad, b.right + pad, square.bottom + pad + _line_height};
		dc.draw_text(c.caption, caption, render::font_face::dialog, render::text_style::single_line_center,
		             c.member == faces::member_kind::suggested ? dim_clr : text_clr, {});

		if (!c.detail.empty())
		{
			dc.draw_text(c.detail, caption.offset(0, _line_height), render::font_face::dialog,
			             render::text_style::single_line_center, dim_clr, {});
		}

		if (static_cast<int>(i) == focus) draw_keyboard_ring(dc, square);
	}
}

std::string faces_view::drag_label() const
{
	if (!_drag.target.valid) return count_faces(_drag.items.size());
	const auto row = _drag.target.row;
	const auto n = std::ranges::count_if(_drag.items, [this, row](int c) { return can_drop_cell(c, row); });
	return std::format("{} \u00b7 {}", str_format(tt.faces_move_to_fmt, _rows[row].title), count_faces(n));
}

void faces_view::draw_drag(render::draw_context& dc) const
{
	if (!_drag.active || _drag.items.empty()) return;

	const auto alpha = dc.colors.alpha;
	const auto edge = core::round(56 * _scale_factor);
	const auto step = core::round(6 * _scale_factor);
	const auto radius = core::round(6 * _scale_factor);
	const auto count = static_cast<int>(std::min<size_t>(3, _drag.items.size()));
	const auto label_width = std::max(1, std::min(_extent.cx - 2 * step, core::round(360 * _scale_factor)));
	const auto label = dc.measure_text(_drag.label, render::font_face::dialog, render::text_style::multiline,
	                                   label_width);
	const auto badge_width = label.cx + 2 * step;
	const auto stack_height = edge + step * count;
	const auto height = stack_height + label.cy + step;
	const pointi at{std::clamp(_drag.loc.x + 2 * step, 0,
	                           std::max(0, _extent.cx - std::max(edge + count * step, badge_width))),
	                _drag.loc.y + height + 2 * step <= _extent.cy ? _drag.loc.y + 2 * step
	                                                              : std::max(0, _drag.loc.y - height - step)};

	for (auto n = count - 1; n >= 0; --n)
	{
		const auto index = _drag.items[n];
		const recti square(at.x + n * step, at.y + n * step, at.x + n * step + edge, at.y + n * step + edge);
		const auto& shown = _drag.from == pane::faces ? _cells[index].shown : _rows[index].face;
		dc.draw_rounded_rect(square.inflate(core::round(2 * _scale_factor)),
		                     core::color(ui::style::colors.view_selected_background, alpha), radius);
		draw_crop(dc, shown, square, alpha, radius);
	}

	const recti badge(at.x, at.y + stack_height, at.x + badge_width, at.y + height);
	dc.draw_rounded_rect(badge, core::color(ui::style::colors.bubble_background, alpha), radius);
	dc.draw_text(_drag.label, badge.inflate(-step, 0), render::font_face::dialog, render::text_style::multiline_center,
	             core::color(ui::style::colors.view_text, alpha), {});
}

recti faces_view::keyboard_bounds() const
{
	if (_pane == pane::groups) return row_bounds(focus_row());
	return cell_bounds(focus_cell());
}

void faces_view::broadcast_event(const view_element_event& event) const
{
	if (event.type == view_element_event_type::update_command_state)
	{
		for (const auto& [id, button] : _panel_commands)
		{
			const auto c = _state.find_command(id);
			button->enable(c && c->enable);
		}
		if (_delete_button) _delete_button->enable(_state.item_index.faces().can_change_book());
	}
	else if (event.type == view_element_event_type::free_graphics_resources)
	{
		for (const auto& [key, crop] : _crops) crop.texture.reset();
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Crops
///////////////////////////////////////////////////////////////////////////////////////////////////

// The faces and rows in view, then those a screen beyond, are planned, each picture once for all its faces, at the
// size the smallest of them needs; those further away are let go, and a plan made again drops what it no longer wants.
int faces_view::crop_edge() const
{
	// Reuse a crop within a size step, rather than asking to decode on every slider movement.
	return std::clamp((_tile + 31) / 32 * 32, 32, 512);
}

void faces_view::request_crops()
{
	if (!_active) return;

	if (!_crop_queue)
	{
		_crop_queue = std::make_shared<face_crop_queue>(_state._async,
		                                                [weak = weak_from_this(),
		                                                 &async = _state._async](const uint64_t generation,
		                                                                         const int edge,
		                                                                         face_crop_queue::cut_crops cut)
		{
			async.queue_ui([weak, generation, edge, cut = std::move(cut)]() mutable
			{
				if (const auto view = weak.lock()) view->crops_cut(generation, edge, std::move(cut));
			});
		});
	}

	struct wanted
	{
		std::string key;
		faces_view_model::shown shown;
		int distance = 0;
	};

	const auto edge = crop_edge();
	core::hash_set<std::string> near;
	std::vector<wanted> wants;

	const auto consider =
		[&](const faces_view_model::shown& s, const recti content, const int scroll_y, const int height)
	{
		if (content.is_empty() || s.path.is_empty()) return;
		const auto top = content.top - scroll_y;
		const auto bottom = content.bottom - scroll_y;
		const auto distance = bottom < 0 ? -bottom : top > height ? top - height : 0;
		if (distance > height * 3) return;

		auto key = crop_key(s);
		near.emplace(key);
		if (distance > height) return;

		const auto found = _crops.find(key);
		if (found != _crops.end() && found->second.requested_edge >= edge) return;
		wants.push_back({std::move(key), s, distance});
	};

	const auto groups_height = std::max(1, groups_pane().height());
	const auto faces_height = std::max(1, faces_pane().height());
	for (size_t i = 0; i < _rows.size(); ++i)
	{
		const auto& r = _rows[i];
		for (const auto& sample : r.previews)
			consider(sample, r.bounds, _groups_scroller.scroll_offset().y, groups_height);
	}
	const auto scroll_y = _faces_scroller.scroll_offset().y;
	const auto near_start = std::ranges::lower_bound(_cells, scroll_y - 3 * faces_height, {},
	                                                 [](const cell& c) { return c.bounds.bottom; });
	for (auto it = near_start; it != _cells.end() && it->bounds.top <= scroll_y + 4 * faces_height; ++it)
		consider(it->shown, it->bounds, scroll_y, faces_height);
	for (const auto& p : _people_review)
		consider(_rows[p.row].face, p.bounds, _faces_scroller.scroll_offset().y, faces_height);

	// A requested bubble takes priority over background crops.
	const auto preview = [&](const faces_view_model::shown& face)
	{
		if (face.path.is_empty()) return;
		auto key = crop_key(face);
		near.emplace(key);
		if (const auto found = _crops.find(key); found == _crops.end() || found->second.requested_edge < edge)
			wants.push_back({std::move(key), face, -1});
	};
	preview(_preview_face);
	if (!_bubble_picture_key.empty()) near.emplace(_bubble_picture_key);

	std::erase_if(_crops, [&near](const auto& entry) { return !near.contains(entry.first); });
	std::ranges::stable_sort(wants, [](const wanted& a, const wanted& b) { return a.distance < b.distance; });

	std::vector<face_crop_queue::job> jobs;
	core::hash_map<core::file_path, size_t, core::ihash, core::ieq> job_of;

	if (!_bubble_picture_key.empty() && _crops[_bubble_picture_key].requested_edge == 0)
	{
		if (const auto ft = _state.item_index.find_item(_bubble_picture).ft)
			jobs.push_back({_bubble_picture,
			                ft,
			                _bubble_picture_edge,
			                {{_bubble_picture_key, {}, _bubble_frame_ms.value_or(0)}},
			                true,
			                _bubble_frame_ms.has_value()});
	}

	for (const auto& w : wants)
	{
		_crops.try_emplace(w.key);

		if (const auto found = job_of.find(w.shown.path); found != job_of.end())
		{
			auto& faces = jobs[found->second].faces;
			if (std::ranges::none_of(faces, [&w](const auto& f) { return f.key == w.key; }))
				faces.push_back({w.key, w.shown.area, w.shown.frame_ms.value_or(0)});
			continue;
		}

		// A picture the index no longer holds keeps its placeholder: there is nothing to read it by.
		const auto ft = _state.item_index.find_item(w.shown.path).ft;
		if (!ft) continue;

		job_of.emplace(w.shown.path, jobs.size());
		jobs.push_back({w.shown.path,
		                ft,
		                edge,
		                {{w.key, w.shown.area, w.shown.frame_ms.value_or(0)}},
		                false,
		                w.shown.frame_ms.has_value()});
	}

	_crop_queue->plan(std::move(jobs), _crop_generation);
}

void faces_view::crops_cut(const uint64_t generation, const int edge,
                           std::vector<std::pair<std::string, core::const_surface_ptr>> cut)
{
	if (!_active || generation != _crop_generation) return;

	const auto preview_key = _preview_face.path.is_empty() ? std::string{} : crop_key(_preview_face);
	auto preview_cut = false;

	for (auto& [key, surface] : cut)
	{
		// One let go meanwhile is cut again if it comes back near.
		const auto found = _crops.find(key);
		if (found == _crops.end() || found->second.requested_edge > edge) continue;
		auto& crop = found->second;
		crop.requested_edge = edge;
		if (surface)
		{
			crop.surface = std::move(surface);
			crop.texture.reset();
		}
		preview_cut = preview_cut || key == preview_key || key == _bubble_picture_key;
	}

	_state.invalidate_view(preview_cut ? view_invalid::view_redraw | view_invalid::tooltip : view_invalid::view_redraw);
	if (edge < crop_edge()) request_crops();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The pointer
///////////////////////////////////////////////////////////////////////////////////////////////////

int faces_view::row_at(const pointi loc) const
{
	if (!groups_pane().contains(loc)) return -1;
	const auto content = loc - groups_offset();
	for (size_t i = 0; i < _rows.size(); ++i)
		if (_rows[i].bounds.contains(content)) return static_cast<int>(i);
	return -1;
}

int faces_view::cell_at(const pointi loc) const
{
	if (!faces_pane().contains(loc)) return -1;
	const auto content = loc - faces_offset();
	for (size_t i = 0; i < _cells.size(); ++i)
		if (_cells[i].bounds.contains(content)) return static_cast<int>(i);
	return -1;
}

int faces_view::heading_at(const pointi loc) const
{
	if (!faces_pane().contains(loc)) return -1;
	const auto content = loc - faces_offset();
	for (size_t i = 0; i < _headings.size(); ++i)
		if (_headings[i].bounds.contains(content)) return static_cast<int>(i);
	return -1;
}

std::vector<int> faces_view::cells_under(const int heading) const
{
	std::vector<int> result;
	if (heading < 0 || heading >= static_cast<int>(_headings.size())) return result;
	const auto first = _headings[heading].before;
	const auto last = heading + 1 < static_cast<int>(_headings.size()) ? _headings[heading + 1].before : _cells.size();
	for (auto i = first; i < last && i < _cells.size(); ++i) result.push_back(static_cast<int>(i));
	return result;
}

std::vector<int> faces_view::cells_in(const recti content) const
{
	std::vector<int> result;
	for (size_t i = 0; i < _cells.size(); ++i)
		if (_cells[i].bounds.intersects(content)) result.push_back(static_cast<int>(i));
	return result;
}

bool faces_view::shown_selected(const int cell, const bool band_has_cells) const
{
	const auto selected = is_cell_selected(cell);
	if (!_marquee.active || !band_has_cells) return selected;
	const auto inside = _cells[cell].bounds.intersects(_marquee.bounds());
	return _marquee.toggle ? selected != inside : inside;
}

faces_view::drop_target faces_view::target_at(const pointi loc) const
{
	drop_target t;
	if (!_drag.active || !groups_pane().contains(loc)) return t;
	t.row = row_at(loc);
	t.list = t.row < 0;
	t.valid = can_drop(_drag.from, _drag.items, t);
	return t;
}

view_controller_ptr faces_view::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                         hit_test_context& ctx)
{
	if (_groups_scroller.can_scroll() && ctx.occluded(_groups_scroller.scroll_bounds()))
		return std::make_shared<ui::scroll_controller>(host, _groups_scroller, _groups_scroller.scroll_bounds());

	if (_faces_scroller.can_scroll() && ctx.occluded(_faces_scroller.scroll_bounds()))
		return std::make_shared<ui::scroll_controller>(host, _faces_scroller, _faces_scroller.scroll_bounds());

	if (ctx.occluded(splitter_bounds()))
		return std::make_shared<faces_splitter_controller>(host, shared_from_this(), splitter_bounds());

	if (!_faces_title_bounds.is_empty() && ctx.occluded(_faces_title_bounds))
	{
		return std::make_shared<faces_press_controller>(host, _faces_title_bounds,
		                                                [view = shared_from_this()] { view->click_faces_title(); });
	}

	if (!_turn_on_bounds.is_empty())
	{
		const auto button = _turn_on_bounds.offset(faces_offset()).intersection(faces_pane());
		if (ctx.occluded(button))
		{
			return std::make_shared<faces_press_controller>(host, button, [view = shared_from_this()]
			{
				turn_on_face_search(view->_state, view->_host->owner());
				view->update_status();
			});
		}
	}

	if (groups_pane().contains(loc))
	{
		ctx.confine(groups_pane());
		const auto offset = groups_offset();
		if (_clear_filter_link->is_visible() && ctx.occluded(_clear_filter_link->bounds.offset(offset)))
			return _clear_filter_link->controller_from_location(host, loc, offset, ctx);
		const auto index = row_at(loc);
		if (index < 0) return nullptr;
		const auto b = row_bounds(index);
		if (const auto expander = expander_bounds(index); ctx.occluded(expander))
		{
			ctx.confine(b);
			return std::make_shared<faces_press_controller>(host, expander,
			                                                [view = shared_from_this(), key = _rows[index].key]
			{ view->toggle_surname(view->find_row(key)); });
		}
		ctx.confine(b);
		return std::make_shared<faces_item_controller>(host, shared_from_this(), pane::groups, index, b,
		                                               _rows[index].title_bounds.offset(groups_offset()));
	}

	if (faces_pane().contains(loc))
	{
		ctx.confine(faces_pane());
		if (reviewing_rows()) return nullptr;
		const auto index = cell_at(loc);

		if (index < 0)
		{
			const auto offset = faces_offset();
			const auto pane_bounds = faces_pane();

			if (const auto heading = heading_at(loc); heading >= 0)
			{
				const auto bounds = _headings[heading].bounds.offset(offset).intersection(pane_bounds);
				ctx.confine(bounds);

				// A button at the end of the heading acts on every face under it.
				const auto& buttons = _headings[heading].buttons;
				for (size_t b = 0; b < buttons.size(); ++b)
				{
					const auto button = buttons[b].bounds.offset(offset).intersection(pane_bounds);
					if (!button.is_empty() && ctx.occluded(button))
					{
						return std::make_shared<faces_heading_button_controller>(host, shared_from_this(), button,
						                                                         heading, static_cast<int>(b));
					}
				}

				return std::make_shared<faces_marquee_controller>(host, shared_from_this(), bounds, heading);
			}

			// Between faces: what stays the answer is the gap the pointer is in, every face and heading in view cut
			// away, so moving onto one tests again.
			for (const auto& c : _cells)
			{
				const auto b = c.bounds.offset(offset);
				if (b.intersects(pane_bounds)) ctx.passed_over(b);
			}
			for (const auto& h : _headings)
			{
				const auto b = h.bounds.offset(offset);
				if (b.intersects(pane_bounds)) ctx.passed_over(b);
			}

			return std::make_shared<faces_marquee_controller>(host, shared_from_this(), pane_bounds, -1);
		}

		const auto b = cell_bounds(index);
		ctx.confine(b);
		if (_cells[index].member == faces::member_kind::suggested && book_editable())
		{
			for (const auto action : {suggestion_action::confirm, suggestion_action::reject, suggestion_action::name})
			{
				const auto button = suggestion_button_bounds(index, action).intersection(faces_pane());
				if (ctx.occluded(button))
					return std::make_shared<faces_item_controller>(host, shared_from_this(), pane::faces, index, button,
					                                               recti{}, action);
			}
		}
		return std::make_shared<faces_item_controller>(host, shared_from_this(), pane::faces, index, b, recti{});
	}

	return nullptr;
}

void faces_view::hover_row(const int index)
{
	if (_row_hover == index) return;
	_row_hover = index;
	_state.invalidate_view(view_invalid::view_redraw);
}

// Confirm at the top left, rule out at the bottom left, and name as someone else at the bottom right, clear of the
// selection's mark at the top right.
recti faces_view::suggestion_button_bounds(const int index, const suggestion_action action) const
{
	const auto cell = _cells[index].bounds.offset(faces_offset());
	const auto pad = core::round(3 * _scale_factor);
	const auto edge = std::min(_tile / 2 - pad, core::round(24 * _scale_factor));
	const auto top = action == suggestion_action::confirm ? cell.top + pad : cell.top + _tile - pad - edge;
	const auto left = action == suggestion_action::name ? cell.right - pad - edge : cell.left + pad;
	return {left, top, left + edge, top + edge};
}

std::vector<faces_view::suggestion_button> faces_view::suggestion_buttons(const int cell) const
{
	std::vector<suggestion_button> result;
	if (cell < 0 || cell >= static_cast<int>(_cells.size()) || _cells[cell].member != faces::member_kind::suggested ||
	    !book_editable())
		return result;
	for (const auto action : {suggestion_action::confirm, suggestion_action::reject, suggestion_action::name})
	{
		const auto b = suggestion_button_bounds(cell, action).intersection(faces_pane());
		result.push_back({action, b.is_empty() ? recti{} : b});
	}
	return result;
}

std::string faces_view::suggestion_action_text(const suggestion_action action) const
{
	switch (action)
	{
	case suggestion_action::confirm: return str_format(tt.faces_confirm_person_fmt, _faces_title);
	case suggestion_action::reject: return str_format(tt.faces_not_person_fmt, _faces_title);
	case suggestion_action::name: break;
	}
	return std::string(tt.faces_name_someone_else);
}

void faces_view::review_suggestion(const int index, const faces_view_model::shown& target,
                                   const std::string_view person, const suggestion_action action)
{
	if (index < 0 || index >= static_cast<int>(_cells.size()) || _review_person != person || !book_editable() ||
	    _pending_model || _drag.active || _marquee.active)
		return;
	const auto& c = _cells[index];
	if (!target.face || c.member != faces::member_kind::suggested || c.shown.path != target.path ||
	    c.shown.face != target.face || c.shown.area != target.area)
		return;
	if (action == suggestion_action::name) name_cells({index}, true);
	else
		apply_placement({index},
		                action == suggestion_action::confirm ? faces::face_action::assign : faces::face_action::reject,
		                person);
}

void faces_view::hover_cell(const int index)
{
	if (_hover == index) return;
	_hover = index;
	_state.invalidate_view(view_invalid::view_redraw);
}

void faces_view::hover_heading(const int index)
{
	if (_heading_hover == index) return;
	_heading_hover = index;
	_state.invalidate_view(view_invalid::view_redraw);
}

void faces_view::hover_heading_button(const int heading, const int button)
{
	const std::pair at{heading, button};
	if (_heading_button_hover == at) return;
	_heading_button_hover = at;
	_state.invalidate_view(view_invalid::view_redraw);
}

void faces_view::bubble_heading_button(const int heading, const int button, view_hover_element& hover) const
{
	if (heading < 0 || heading >= static_cast<int>(_headings.size()) || button < 0 ||
	    button >= static_cast<int>(_headings[heading].buttons.size()))
		return;
	const auto action = _headings[heading].buttons[button].action;
	const auto faces = count_faces(cells_under(heading).size());
	hover.elements->add(
		std::make_shared<text_element>(std::format("{} \u00b7 {}", heading_action_text(action), faces)));
	const auto hint = action == heading_action::name   ? tt.faces_name_all_hint
	                  : action == heading_action::hide ? tt.faces_hide_all_hint
	                                                   : tt.faces_confirm_all_hint;
	hover.elements->add(std::make_shared<text_element>(hint, render::font_face::dialog, render::text_style::multiline,
	                                                   flex_item::new_line));
}

void faces_view::click_row(const int index, const platform::key_state keys, const bool on_title)
{
	if (index < 0 || index >= static_cast<int>(_rows.size())) return;
	_keyboard_used = false;
	_pane = pane::groups;
	const auto& r = _rows[index];

	if (r.what == row::kind::surname)
	{
		_people_selection.move_to(r.key, true, false, {});
		toggle_surname(index);
		return;
	}

	// A click on the name of the group shown alone renames it.
	if (on_title && !keys.control && !keys.shift && _people_selection.size() == 1 &&
	    _people_selection.is_selected(r.key) && can_rename_row(index))
	{
		changed(false);
		rename();
		return;
	}

	// By the rules every list selects by: a click one group, Ctrl one more or one less, Shift the run from the last
	// clicked over the groups shown, and Ctrl with Shift that run added.
	const auto before = _people_selection.selected();
	_people_selection.click(r.key, keys.control, keys.shift, selectable_row_keys());

	// The groups already selected, clicked again, become what the commands act on; their faces stay as they are,
	// still selected and scrolled where they were.
	if (_people_selection.selected() == before)
	{

		changed(false);
		return;
	}

	show_selected_rows();
}

void faces_view::double_click_row(const int index)
{
	if (index >= 0 && index < static_cast<int>(_rows.size()) && can_rename_row(index))
	{
		_pane = pane::groups;
		_people_selection.click(_rows[index].key, false, false, {});
		show_selected_rows();
		rename();
	}
}

void faces_view::click_cell(const int index, const platform::key_state keys)
{
	if (index < 0 || index >= static_cast<int>(_cells.size())) return;
	_keyboard_used = false;

	_pane = pane::faces;

	// Selected as items are, by the rules every list selects by: a click one face, Ctrl one more or one less, Shift
	// the run from the last clicked, and Ctrl with Shift that run added.
	_face_selection.click(_cell_keys[index], keys.control, keys.shift, _cell_keys);
	changed(false);
}

void faces_view::click_heading(const int index, const platform::key_state keys)
{
	const auto under = cells_under(index);
	if (under.empty()) return;
	_keyboard_used = false;

	_pane = pane::faces;

	std::vector<std::string> chosen;
	for (const auto c : under) chosen.push_back(_cell_keys[c]);
	using choice = ui::list_selection<std::string>::choice;
	_face_selection.choose(chosen, keys.control ? choice::add : choice::replace);
	changed(false);
}

void faces_view::double_click_cell(const int index)
{
	if (index < 0 || index >= static_cast<int>(_cells.size())) return;
	const auto& shown = _cells[index].shown;

	// docs/faces.md#93-faces-on-a-picture: a video's face opens the video, paused on the frame the face was found in, the
	// face outlined. Its time was kept to the millisecond, so the frame asked for is just past it, which the key frame
	// it is lands on whichever way the millisecond was rounded.
	if (shown.frame_ms && shown.face)
	{
		core::unique_paths selection;
		selection.emplace(shown.path);
		if (!_state.open(_host, df::search_t().add_selector(shown.path.folder()), selection)) return;
		if (_state.history.current_view() == view_type::faces)
			_state.history.history_add(df::search_t().add_selector(shown.path.folder()), _state.selected_items().ids());
		_state._pending_media_start = {shown.path, *shown.frame_ms / 1000.0 + 0.0005, *shown.face, core::now_ms()};
		_state.view_mode(view_type::media);
		return;
	}

	open_picture_in_folder(shown.path);
}

void faces_view::open_picture_in_folder(const core::file_path path)
{
	core::unique_paths selection;
	selection.emplace(path);
	open_items(df::search_t().add_selector(path.folder()), selection);
}

void faces_view::click_faces_title()
{
	// The title names the group shown, whatever is selected among its faces.
	if (_faces_title_bounds.is_empty()) return;
	_pane = pane::groups;
	changed(false);
	rename();
}

void faces_view::hold_rows()
{
	_holding_rows = true;
}

void faces_view::release_rows()
{
	if (!_holding_rows && !_pending_model && !_rows_need_build && !_build_again) return;
	_holding_rows = false;
	if (!_active)
	{
		_pending_model.reset();
		return;
	}
	if (_pending_model)
	{
		auto model = std::exchange(_pending_model, {});
		model_built(std::move(model), _model_request);
	}
	else
	{
		if (_rows_need_build) build_rows();
		if (_build_again && !_building)
		{
			_build_again = false;
			request_model();
		}
	}
}

void faces_view::begin_drag(const pane from, const int index, const pointi loc)
{
	_drag = {};
	_drag_scroll.reset();
	std::vector<int> items;

	if (from != pane::faces) return;
	{
		if (index < 0 || index >= static_cast<int>(_cells.size())) return;

		// What is pressed is what moves: the selection where the press was on it, otherwise the face pressed alone,
		// which is selected so it can be seen going.
		if (!is_cell_selected(index)) _face_selection.click(_cell_keys[index], false, false, _cell_keys);

		items = selected_cells();
		std::erase_if(items, [this](const int i) { return !_cells[i].shown.face; });
	}

	if (items.empty()) return;
	_pane = from;
	_drag.active = true;
	_drag.from = from;
	_drag.items = std::move(items);
	_drag.loc = loc;
	_drag.target = target_at(loc);
	_drag.label = drag_label();
	changed(false);
}

void faces_view::drag_to(const pointi loc)
{
	if (!_drag.active) return;
	_drag.loc = loc;

	// Near the top or the bottom of the People pane, it carries the groups along at the pace a drag carries every
	// list, so every group can be reached; over the faces beside it, it carries nothing.
	const auto pane = groups_pane();
	const auto over = loc.x >= pane.left && loc.x < pane.right;
	if (const auto step = over ? _drag_scroll.step(loc.y, pane.top, pane.bottom, _scale_factor) : 0; step != 0 && _host)
		_groups_scroller.offset(_host, 0, step);
	if (!over) _drag_scroll.reset();

	const auto target = target_at(loc);
	if (target.row != _drag.target.row || target.list != _drag.target.list || target.valid != _drag.target.valid)
	{
		_drag.target = target;
		_drag.label = drag_label();
	}
	_state.invalidate_view(view_invalid::view_redraw);
}

void faces_view::end_drag(const pointi loc)
{
	if (!_drag.active) return;
	const auto target = target_at(loc);
	const auto same_target = target.row == _drag.target.row && target.list == _drag.target.list;
	const auto from = _drag.from;
	auto items = std::move(_drag.items);
	const auto was_holding = _holding_rows;
	hold_rows();
	const core::scope_exit release([&]
	{
		if (!was_holding) release_rows();
	});
	_drag = {};
	_state.invalidate_view(view_invalid::view_redraw);
	if (!target.valid || !same_target) return;
	if (_book_generation == _state.item_index.faces().book().generation()) drop(from, items, target);
	else core::log(__FUNCTION__, "Drop cancelled because the face book changed during the drag.");
}

void faces_view::cancel_drag()
{
	if (!_drag.active) return;
	_drag = {};
	_state.invalidate_view(view_invalid::view_redraw);
	if (!_holding_rows) release_rows();
}

void faces_view::begin_marquee(const pointi loc, const bool toggle)
{
	_marquee.active = true;
	_marquee.toggle = toggle;
	_marquee.start = _marquee.end = loc - faces_offset();
	_band_scroll.reset();
	_keyboard_used = false;

	_pane = pane::faces;
	_state.invalidate_view(view_invalid::view_redraw);
}

void faces_view::marquee_to(const pointi loc)
{
	if (!_marquee.active) return;
	// In the content, so a band held while the pane scrolls keeps its start where it was pressed.
	_marquee.end = loc - faces_offset();
	_state.invalidate_view(view_invalid::view_redraw);
}

void faces_view::end_marquee(const pointi loc)
{
	if (!_marquee.active) return;
	marquee_to(loc);
	const auto inside = cells_in(_marquee.bounds());
	const auto toggle = _marquee.toggle;
	_marquee = {};

	// A band that touches no face leaves the selection as it was, as an empty one does in Items; one that does takes
	// them as Items' does, or with Ctrl puts each in or out of the selection.
	std::vector<std::string> chosen;
	for (const auto c : inside) chosen.push_back(_cell_keys[c]);
	using choice = ui::list_selection<std::string>::choice;
	_face_selection.choose(chosen, toggle ? choice::toggle : choice::replace);

	changed(false);
}

void faces_view::cancel_marquee()
{
	if (!_marquee.active) return;
	_marquee = {};
	_state.invalidate_view(view_invalid::view_redraw);
}

// The band follows the pointer, and held near the top or the bottom of the pane carries the faces along at the pace a
// drag carries every list; the pointer is still while they move under it, so the band's end follows them.
void faces_view::marquee_scroll(const pointi loc)
{
	if (!_marquee.active) return;
	const auto pane = faces_pane();
	if (const auto step = _band_scroll.step(loc.y, pane.top, pane.bottom, _scale_factor); step != 0 && _host)
		_faces_scroller.offset(_host, 0, step);
	marquee_to(loc);
}

void faces_view::search_tooltip(view_hover_element& hover, const commands id)
{
	const auto add_line = [&hover](const std::string_view text)
	{
		hover.elements->add(std::make_shared<text_element>(text, render::font_face::dialog,
		                                                   render::text_style::multiline, flex_item::new_line));
	};
	if (id == commands::face_search)
	{
		if (const auto* const face = face_search_target())
		{
			add_crop_preview(hover, *face);
			add_line(face->path.name());
		}
	}
	else if (id == commands::face_person_search)
	{
		if (const auto name = person_search_target(); !name.empty())
			add_line(str_format(tt.faces_person_search_tooltip_fmt, name));
	}
	else
	{
		if (faces_targeted())
		{
			for (const auto c : selected_cells())
			{
				const auto& cell = _cells[c];
				if (id == commands::face_show_again && !cell.hidden) continue;
				if (id == commands::face_hide && (!cell.shown.face || cell.hidden)) continue;
				if (id == commands::face_reject && !cell.shown.face) continue;
				add_crop_preview(hover, cell.shown);
				break;
			}
		}
		else
		{
			for (const auto r : selected_rows())
			{
				const auto& row = _rows[r];
				if (row.face.path.is_empty()) continue;
				add_crop_preview(hover, row.face);
				break;
			}
		}
		if (!_selection_title.empty()) add_line(_selection_title);
		if (id == commands::face_confirm) add_line(_selection_detail);
		if (id == commands::face_rename_person)
		{
			const auto rows = selected_rows();
			const auto file_only = rows.size() == 1 && _rows[rows.front()].what == row::kind::file_person;
			add_line(file_only ? tt.faces_file_person_hint.sv() : tt.faces_review_hint.sv());
		}
		else if (id == commands::face_name) add_line(tt.faces_name_face_hint);
	}
}

void faces_view::bubble_row(const int index, view_hover_element& hover)
{
	if (index < 0 || index >= static_cast<int>(_rows.size())) return;
	const auto& r = _rows[index];

	hover.active_delay = bubble_delay::face_preview;
	hover.elements->add(std::make_shared<text_element>(r.title, render::font_face::title, render::text_style::multiline,
	                                                   flex_item::line_break));
	if (!r.detail.empty())
	{
		hover.elements->add(std::make_shared<text_element>(r.detail, render::font_face::dialog,
		                                                   render::text_style::multiline, flex_item::new_line));
	}

	const auto about = r.named() ? tt.faces_rename_hint.sv() : std::string_view{};
	if (!about.empty())
	{
		hover.elements->add(std::make_shared<text_element>(about, render::font_face::dialog,
		                                                   render::text_style::multiline, flex_item::new_line));
	}

	hover.active_bounds = hover.window_bounds = row_bounds(index);
	hover.horizontal = true;
}

void faces_view::bubble_cell(const int index, view_hover_element& hover)
{
	if (index < 0 || index >= static_cast<int>(_cells.size())) return;
	const auto& c = _cells[index];

	hover.active_delay = bubble_delay::face_preview;
	hover.active_bounds = hover.window_bounds = cell_bounds(index);
	hover.horizontal = true;
	// The app's client area (including the inspector), in pixels already scaled for this display.
	const auto app_area = _host && !_host->_extent.is_empty() ? _host->_extent : _extent;
	const sizei photo_limit{std::max(1, app_area.cx / 3), std::max(1, app_area.cy / 3)};
	hover.preferred_size = std::max(1, core::round(photo_limit.cx / _scale_factor));
	hover.elements->flex_container.direction = flex_direction::column;
	hover.elements->flex_container.wrap = flex_wrap::no_wrap;
	hover.elements->flex_container.align_items = flex_align::start;

	const auto who = !_review_person.empty() ? std::string_view(_review_person)
	                 : c.hidden              ? tt.faces_hidden_section.sv()
	                                         : tt.unknown.sv();
	hover.elements->add(std::make_shared<text_element>(who, render::font_face::title, render::text_style::multiline,
	                                                   view_element_style::none));
	if (c.member == faces::member_kind::suggested)
	{
		// docs/faces.md#72-suggestions: a resemblance is not a probability, so no score is shown as one.
		auto suggested = std::make_shared<text_element>(tt.faces_suggested);
		suggested->foreground_color(ui::style::tone_text(ui::style::tone::accent));
		hover.elements->add(suggested);
	}

	if (!c.shown.path.is_empty())
	{
		// The whole picture: clothes, place and company often tell who a face is when the face alone does not. Kept for
		// the few pictures last pointed at, which the bubble asks for again as it is drawn while the thumbnail loads; a
		// pointer passing over every face of a long list does not keep every picture it passed.
		constexpr size_t pictures_kept = 16;
		if (_pictures.size() >= pictures_kept && !_pictures.contains(c.shown.path)) _pictures.clear();
		auto& item = _pictures[c.shown.path];

		if (!item)
		{
			const auto indexed = _state.item_index.find_item(c.shown.path);
			if (indexed.ft) item = std::make_shared<df::item_element>(c.shown.path, indexed);
		}

		const auto md = item ? item->metadata() : nullptr;
		if (item)
		{
			// Share the whole-photo decode between faces of one picture. The overlay is drawn for the
			// current face, never baked into the cached photo or delivered by a previous hover. A video's face is shown
			// in the frame it was found in: a key frame, read and decoded alone.
			const auto edge = std::max(photo_limit.cx, photo_limit.cy);
			const auto decode_edge = std::min(((edge + 31) / 32) * 32, faces::defaults().decode_edge);
			auto key = std::format("photo:{}:{}:{}:{}", _crop_generation, item->file_modified().to_int64(), decode_edge,
			                       c.shown.path.str());
			if (c.shown.frame_ms) key += std::format("@{}", *c.shown.frame_ms);
			if (key != _bubble_picture_key)
			{
				_bubble_picture = c.shown.path;
				_bubble_picture_edge = decode_edge;
				_bubble_picture_key = std::move(key);
				_bubble_frame_ms = c.shown.frame_ms;
				request_crops();
			}

			// Until the frame is read a video shows its thumbnail, another frame, with no face marked on it.
			auto surface = item->thumbnail_surface();
			auto area = c.shown.frame_ms ? rectd{} : c.shown.area;
			if (const auto ready = _crops.find(_bubble_picture_key); ready != _crops.end() && ready->second.surface)
			{
				surface = ready->second.surface;
				area = c.shown.area;
			}
			const auto has_surface = is_valid(surface);
			const auto upright = has_surface ? faces::upright_extent(surface->dimensions(), surface->orientation())
			                     : md        ? faces::upright_extent(md->dimensions(), md->orientation)
			                                 : sizei{};
			if (!upright.is_empty())
			{
				const auto fitted = core::scale_dimensions(upright, photo_limit);
				hover.preferred_size = std::max(1, core::round(fitted.cx / _scale_factor));
			}
			if (has_surface) hover.elements->add(std::make_shared<surface_element>(surface, photo_limit, area));
			else if (is_valid(item->thumbnail())) item->stage_thumbnail_surface(_state._async, false, true);
			else _state.item_index.queue_load_thumbnail(item);
		}

		// A video's face says where in the video it is.
		const auto name = c.shown.frame_ms
		                      ? std::format("{} \u00b7 {}", c.shown.path.name(),
		                                    core::format_seconds(static_cast<int>(*c.shown.frame_ms / 1000)))
		                      : std::string(c.shown.path.name());
		hover.elements->add(std::make_shared<text_element>(name, render::text_style::multiline));

		// When and where it was taken, which tell who a face is as often as the face does.
		if (item)
		{
			const auto date = platform::format_date(item->media_created());
			const auto place = md ? place_of(_state, *md) : std::string{};
			if (!date.empty() || !place.empty())
			{
				const auto detail = date.empty()    ? place
				                    : place.empty() ? date
				                                    : std::format("{} \u00b7 {}", date, place);
				auto line = std::make_shared<text_element>(detail, render::text_style::multiline);
				line->foreground_color(ui::style::tone_text(ui::style::tone::muted));
				hover.elements->add(line);
			}
		}
	}
}

bool faces_view::mouse_wheel(const pointi loc, const platform::wheel_notch notch)
{
	if (!notch.is_vertical()) return false;
	auto& scroller = loc.x < _groups_width ? _groups_scroller : _faces_scroller;
	scroller.offset(_host, 0, -notch.delta);
	return scroller.can_scroll();
}

bool faces_view::can_scroll(const pointi loc) const
{
	return loc.x < _groups_width ? _groups_scroller.can_scroll() : _faces_scroller.can_scroll();
}

void faces_view::ensure_row_visible(const int row)
{
	if (row < 0 || row >= static_cast<int>(_rows.size()) || !_host) return;
	scroll_into_view(_groups_scroller, _host, _rows[row].bounds.offset(groups_offset()));
}

void faces_view::ensure_visible(const int cell)
{
	if (cell < 0 || cell >= static_cast<int>(_cells.size()) || !_host) return;
	scroll_into_view(_faces_scroller, _host, _cells[cell].bounds.offset(faces_offset()));
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The keyboard
///////////////////////////////////////////////////////////////////////////////////////////////////

// Up and Down move among the groups, selecting as they go - Shift extends the selection - and Right goes to their
// faces; among the faces the arrows move as among items, and Left from the first of a row goes back to the groups.
// F2 names faces or renames a person, Delete rejects the person shown, C confirms the suggestions selected, H hides
// the faces selected - or shows them again where they are all hidden - and Enter opens a face's picture.
bool faces_view::key_down(const char32_t key, const platform::key_state keys)
{
	if (keys.alt) return false;

	if (key == platform::keys::F2)
	{
		if (can_name_faces()) name_faces();
		else if (can_rename()) rename();
		return true;
	}

	// Delete rejects the match. It never hides faces or deletes a file.
	if (key == platform::keys::DEL)
	{
		if (!keys.control && !keys.shift && can_reject()) reject();
		return true;
	}

	if (!keys.control && !keys.shift && (key == U'C' || key == U'c'))
	{
		if (can_confirm()) confirm();
		return true;
	}

	if (!keys.control && !keys.shift && (key == U'H' || key == U'h'))
	{
		if (can_hide()) hide(true);
		else if (can_show_again()) hide(false);
		return true;
	}

	return _pane == pane::groups ? key_down_groups(key, keys) : key_down_faces(key, keys);
}

bool faces_view::key_down_groups(const char32_t key, const platform::key_state keys)
{
	if (_rows.empty()) return false;

	if (const int row = focus_row(); row >= 0 && _rows[row].what == row::kind::surname &&
	                                 (key == platform::keys::LEFT || key == platform::keys::RIGHT ||
	                                  key == platform::keys::RETURN || key == platform::keys::SPACE))
	{
		if (key == platform::keys::RETURN || key == platform::keys::SPACE ||
		    (key == platform::keys::RIGHT) == surname_collapsed(_rows[row].title))
			toggle_surname(row);
		return true;
	}
	if (key == platform::keys::RIGHT || key == platform::keys::RETURN)
	{
		if (_cells.empty()) return true;
		if (focus_cell() < 0) click_cell(0, {});
		_pane = pane::faces;
		_keyboard_used = true;
		ensure_visible(focus_cell());
		changed(false);
		return true;
	}

	const auto count = static_cast<int>(_rows.size());
	const auto focus = focus_row();
	auto next = focus;

	if (key == platform::keys::SPACE)
	{
		if (focus >= 0) click_row(focus, {.control = true}, false);
		_keyboard_used = true;
		return true;
	}

	const auto page = std::max(1, groups_pane().height() / std::max(1, _row_height) - 1);
	next = ui::step_list_cursor(focus, count, key, page, false);
	if (next < 0) return false;
	const auto step = key == platform::keys::UP || key == platform::keys::PRIOR || key == platform::keys::END ? -1 : 1;
	while (next >= 0 && next < count && _rows[next].bounds.is_empty()) next += step;
	if (next < 0 || next >= count) return true;

	_keyboard_used = true;

	// By the rules every list selects by: the cursor picks out the group it reaches, Shift extends the selection from
	// the anchor over the groups shown and Ctrl with Shift adds that run, and Ctrl alone moves the cursor - as does
	// reaching the hidden faces' section, which is no group.
	const auto group = _rows[next].what != row::kind::surname;
	const auto before = _people_selection.selected();
	_people_selection.move_to(_rows[next].key, keys.control || !group, keys.shift && group, selectable_row_keys());
	if (_people_selection.selected() != before) show_selected_rows();

	ensure_row_visible(next);
	changed(false);
	return true;
}

bool faces_view::key_down_faces(const char32_t key, const platform::key_state keys)
{
	if (_cells.empty()) return false;

	const auto focus = focus_cell();

	if (key == platform::keys::RETURN)
	{
		if (focus >= 0) double_click_cell(focus);
		return true;
	}

	if (key == platform::keys::SPACE)
	{
		if (focus >= 0) click_cell(focus, {.control = true});
		_keyboard_used = true;
		return true;
	}

	const auto count = static_cast<int>(_cells.size());
	auto next = focus;

	if (focus < 0 || key == platform::keys::HOME)
	{
		next = 0;
	}
	else if (key == platform::keys::END)
	{
		next = count - 1;
	}
	else if (key == platform::keys::LEFT || key == platform::keys::RIGHT)
	{
		const auto candidate = focus + (key == platform::keys::LEFT ? -1 : 1);

		// Left from the first face of a row goes back to the groups.
		if (key == platform::keys::LEFT && (candidate < 0 || _cells[candidate].bounds.top != _cells[focus].bounds.top))
		{
			_pane = pane::groups;
			_keyboard_used = true;
			changed(false);
			return true;
		}

		next = std::clamp(candidate, 0, count - 1);
	}
	else if (key == platform::keys::UP || key == platform::keys::DOWN)
	{
		// The nearest face in the row above or below.
		const auto& from = _cells[focus].bounds;
		auto best = -1;
		auto best_distance = INT_MAX;

		for (auto i = 0; i < count; ++i)
		{
			const auto& b = _cells[i].bounds;
			const auto is_beyond = key == platform::keys::UP ? b.bottom <= from.top : b.top >= from.bottom;
			if (!is_beyond) continue;
			const auto distance = std::abs(b.top - from.top) * 4 + std::abs(b.left - from.left);
			if (distance < best_distance)
			{
				best = i;
				best_distance = distance;
			}
		}

		if (best < 0) return true;
		next = best;
	}
	else if (key == platform::keys::PRIOR || key == platform::keys::NEXT)
	{
		// A page: the face nearest where this one would be, a pane's height up or down.
		const auto& from = _cells[focus].bounds;
		const auto travel = std::max(_tile, faces_pane().height() - _tile);
		const auto target_top = from.top + (key == platform::keys::PRIOR ? -travel : travel);
		auto best_distance = INT_MAX;

		for (auto i = 0; i < count; ++i)
		{
			const auto& b = _cells[i].bounds;
			const auto distance = std::abs(b.top - target_top) * 4 + std::abs(b.left - from.left);
			if (distance < best_distance)
			{
				next = i;
				best_distance = distance;
			}
		}
	}
	else
	{
		return false;
	}

	// By the rules every list selects by: the cursor picks out the face it reaches, Shift extends the selection from
	// the anchor and Ctrl with Shift adds that run, and Ctrl alone moves the cursor.
	_keyboard_used = true;
	_face_selection.move_to(_cell_keys[next], keys.control, keys.shift, _cell_keys);

	ensure_visible(next);
	changed(false);
	return true;
}

bool faces_view::escape()
{
	if (_drag.active)
	{
		cancel_drag();
		return true;
	}

	return false;
}

void faces_view::exit()
{
	_state.view_mode(view_type::items);
}

std::string_view faces_view::title()
{
	_title = std::format("{}: {}", core::app_name(), tt.command_faces);
	return _title;
}

std::string_view faces_view::status()
{
	if (can_undo_placement()) return _last_placement;
	return faces_targeted() || reviewing_rows() ? _selection_title : _status;
}

view_bars faces_view::bars() const
{
	auto result = history_view_bars({commands::face_search, commands::face_person_search, commands::face_show_again,
	                                 commands::face_hide, commands::face_rename_person, commands::face_name,
	                                 commands::face_reject, commands::face_confirm});
	result.bottom_leading = {bar_control::face_filter, bar_control::face_size};
	return result;
}

view_menu faces_view::context_menu(const pointi loc)
{
	// As Items does an item, a face or a group right-clicked is selected first, where it is not already, so what the
	// menu does is plain: what is under the pointer, and what is selected with it.
	if (const auto cell = cell_at(loc); cell >= 0)
	{
		if (!is_cell_selected(cell)) click_cell(cell, {});
		_pane = pane::faces;
		changed(false);
		return {.list = {commands::face_open_in_folder, commands::none, commands::face_name, commands::face_confirm,
		                 commands::face_reject, commands::face_hide, commands::face_show_again, commands::none,
		                 commands::face_search, commands::none, commands::view_close}};
	}

	if (const auto row = row_at(loc); row >= 0 && _rows[row].what != row::kind::surname)
	{
		if (!is_row_selected(row)) click_row(row, {}, false);
		_pane = pane::groups;
		changed(false);
		return {
			.list = {commands::face_rename_person, commands::face_person_search, commands::none, commands::view_close}};
	}

	return view_base::context_menu(loc);
}
