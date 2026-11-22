// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The Explore view: its globe of the collection's places, zoomed to street level over map
// tiles, beside its timeline of the collection read from today back, each pointed at marking the other,
// and each part opening its items in Items. The model behind it is model_explore.cpp's; the globe's
// pixels are render_globe.cpp's.

#include "pch.h"

#include "view_explore.h"
#include "app_commands.h"
#include "model_index.h"
#include "model_locations.h"
#include "files.h"
#include "ui_elements.h"
#include "libdesktop/ui/table_element.h"

namespace
{
	// The most map tiles the globe keeps decoded: a little more than a large window shows at once.
	constexpr size_t explore_tile_capacity = 320;

	// The globe is zoomed in steps of a third of a tile level a wheel notch, so three notches show the next
	// level's streets.
	constexpr double explore_zoom_per_notch = 1.2599210498948732;

	// The colour a time pointed at marks its places in, and the places it does not hold fade to.
	const core::color explore_mark_color(0.98f, 0.74f, 0.22f, 1.0f);

	core::color with_alpha(const core::color32 c, const float alpha)
	{
		return core::color(c, alpha);
	}

	// One hue for each place, the same every time it is drawn.
	core::color place_color(const df::explore_place& place)
	{
		uint32_t hash = 2166136261u;
		for (const auto c : place.country.sv()) hash = (hash ^ static_cast<uint8_t>(core::to_lower(c))) * 16777619u;
		for (const auto c : place.state.sv()) hash = (hash ^ static_cast<uint8_t>(core::to_lower(c))) * 16777619u;

		const auto hue = static_cast<float>(hash % 360u) / 60.0f;
		const auto sector = static_cast<int>(hue);
		const auto f = hue - static_cast<float>(sector);
		constexpr auto v = 0.88f;
		constexpr auto s = 0.55f;
		const auto p = v * (1.0f - s);
		const auto q = v * (1.0f - s * f);
		const auto t = v * (1.0f - s * (1.0f - f));

		switch (sector % 6)
		{
		case 0: return {v, t, p, 1.0f};
		case 1: return {q, v, p, 1.0f};
		case 2: return {p, v, t, 1.0f};
		case 3: return {p, q, v, 1.0f};
		case 4: return {t, p, v, 1.0f};
		default: return {v, p, q, 1.0f};
		}
	}

	std::string abbreviated_count(const uint32_t count)
	{
		if (count >= 100000) return std::format("{}k", count / 1000);
		if (count >= 1000) return std::format("{:.1f}k", count / 1000.0);
		return core::to_string(count);
	}

	int marker_radius(const uint32_t count, const double scale)
	{
		const auto r = 8.0 + 2.2 * std::log2(static_cast<double>(std::max(1u, count)) + 1.0);
		return core::round(std::min(r, 22.0) * scale);
	}

	std::string format_day(const uint32_t day, const bool with_year)
	{
		const auto d = df::explore_date(day);
		return with_year ? std::format("{} {} {}", d.day, core::month(d.month, true), d.year)
		                 : std::format("{} {}", d.day, core::month(d.month, true));
	}

	// `3 November 2025`, `3 November to 9 November 2025`, `29 December 2025 to 4 January 2026`.
	std::string format_days(const uint32_t first, const uint32_t last)
	{
		if (first == last) return format_day(first, true);
		const auto same_year = df::explore_date(first).year == df::explore_date(last).year;
		return str_format(tt.explore_range_fmt.sv(), format_day(first, !same_year), format_day(last, true));
	}

	// `5 July to 19 July`, for a label drawn under its year's header; across a new year, with the years.
	std::string format_days_in_year(const uint32_t first, const uint32_t last)
	{
		const auto same_year = df::explore_date(first).year == df::explore_date(last).year;
		if (first == last) return format_day(first, !same_year);
		return str_format(tt.explore_range_fmt.sv(), format_day(first, !same_year), format_day(last, !same_year));
	}

	std::string format_month(const int year, const int month)
	{
		return std::format("{} {}", core::month(month, true), year);
	}

	// The months a gap folds away: `March`, or `March to June`.
	std::string format_months(const uint32_t first_day, const uint32_t last_day)
	{
		const auto first = df::explore_date(first_day);
		const auto last = df::explore_date(last_day);
		if (first.month == last.month) return std::string(core::month(first.month, true));
		return str_format(tt.explore_range_fmt.sv(), core::month(first.month, true), core::month(last.month, true));
	}

	std::string format_years(const int newest, const int oldest)
	{
		if (newest == oldest) return core::to_string(newest);
		return str_format(tt.explore_range_fmt.sv(), core::to_string(oldest), core::to_string(newest));
	}

	// Map tiles for the globe, read from the view's cache, and the tiles the drawing wanted, which are what
	// the cache fetches next. Read from the renderer's bands at once; told what they wanted afterwards.
	class explore_tile_source final : public globe_tile_source
	{
		const map_tile_cache& _cache;
		const int _max_level;
		core::hash_set<int64_t> _seen;

	public:
		std::vector<map_tile_id> wanted;
		mutable std::atomic_bool drawn = false;

		explore_tile_source(const map_tile_cache& cache, const int max_level) : _cache(cache), _max_level(max_level) {}

		int max_level() const override { return _max_level; }

		core::const_surface_ptr tile(const int z, const int x, const int y) const override
		{
			auto surface = _cache.surface({x, y, z});
			if (surface) drawn.store(true, std::memory_order_relaxed);
			return surface;
		}

		void want(const int z, const int x, const int y) override
		{
			if (_seen.insert(map_tile_db_key(z, x, y)).second) wanted.push_back({x, y, z});
		}
	};

	// Routes the pointer in one half of the view to the view, which keeps every bit of state. It stands for
	// its whole half, so it lives as long as the pointer stays there, and says when what it is over changes.
	class explore_controller final : public view_controller
	{
		std::shared_ptr<explore_view> _view;
		explore_view::pane _pane;
		bool _pressed = false;
		bool _moved = false;

	public:
		explore_controller(const view_host_ptr& host, std::shared_ptr<explore_view> view, const explore_view::pane p,
		                   const recti bounds) : view_controller(host, bounds), _view(std::move(view)), _pane(p)
		{
		}

		~explore_controller() override
		{
			if (_pressed) _view->cancel(_pane);
			_view->leave(_pane);
		}

		platform::cursor cursor() const override
		{
			if (_pane == explore_view::pane::globe && _pressed) return platform::cursor::hand_up;
			if (!_view->hovered().is_none()) return platform::cursor::link;
			return _pane == explore_view::pane::globe ? platform::cursor::hand_down : platform::cursor::normal;
		}

		void on_mouse_left_button_down(const pointi loc, const platform::key_state keys) override
		{
			view_controller::on_mouse_left_button_down(loc, keys);
			_pressed = true;
			_moved = false;
			_view->press(_pane, loc);
			_host->invalidate(ui::host_invalid::tooltip);
		}

		void on_mouse_move(const pointi loc) override
		{
			_last_loc = loc;

			if (_pressed)
			{
				_moved |= ui::is_drag(_start_loc, loc);
				_view->drag(_pane, _start_loc, loc, _moved);
				return;
			}

			_view->hover(_pane, loc);
			if (_pane == explore_view::pane::timeline) _view->tick_hover_turn(loc, core::tick_count64());
		}

		// A release counts only after its own press: the one after a press that closed a menu never had one.
		void on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/) override
		{
			_last_loc = loc;
			if (!std::exchange(_pressed, false)) return;
			_view->release(_pane, loc, _moved);
			_host->invalidate(ui::host_invalid::tooltip);
		}

		bool escape() override
		{
			if (!_pressed) return false;
			_pressed = false;
			_host->invalidate(ui::host_invalid::tooltip);
			return _view->cancel(_pane);
		}

		void tick() override
		{
			if (!_pressed && _pane == explore_view::pane::timeline)
				_view->tick_hover_turn(_last_loc, core::tick_count64());
		}

		void popup_from_location(view_hover_element& hover) override
		{
			if (!_pressed) _view->bubble(_pane, hover);
		}
	};
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The timeline's geometry
///////////////////////////////////////////////////////////////////////////////////////////////////

const explore_timeline_layout::row* explore_timeline_layout::row_at(const int y) const
{
	const auto found = std::ranges::upper_bound(rows, y, {}, &row::top);
	if (found == rows.begin()) return nullptr;
	return &*std::prev(found);
}

std::optional<uint32_t> explore_timeline_layout::day_at(const int y) const
{
	const auto* const r = row_at(y);
	if (!r || r->kind != row_kind::days || y >= r->bottom) return {};

	const auto back = static_cast<uint32_t>((y - r->top) / std::max(1, day_height));
	if (back > r->last_day - r->first_day) return {};
	return r->last_day - back;
}

std::optional<int> explore_timeline_layout::top_of(const uint32_t day) const
{
	for (const auto& r : rows)
	{
		if (r.kind == row_kind::days && day >= r.first_day && day <= r.last_day)
		{
			return r.top + static_cast<int>(r.last_day - day) * day_height;
		}
	}

	return {};
}

std::optional<uint32_t> explore_timeline_layout::day_near(const int y) const
{
	const auto* const at = row_at(y);
	const auto first = at ? static_cast<size_t>(at - rows.data()) : 0_z;

	for (auto i = first; i < rows.size(); ++i)
	{
		const auto& r = rows[i];
		if (r.kind != row_kind::days) continue;
		if (r.top > y) return r.last_day;

		const auto back = static_cast<uint32_t>(std::max(0, y - r.top) / std::max(1, day_height));
		if (back <= r.last_day - r.first_day) return r.last_day - back;
	}

	return {};
}

std::optional<int> explore_timeline_layout::top_of_or_older(const uint32_t day) const
{
	// Rows run from the newest, so the first wholly older than the day holds the nearest older one, at its top.
	for (const auto& r : rows)
	{
		if (r.kind != row_kind::days) continue;
		if (day >= r.first_day && day <= r.last_day) return r.top + static_cast<int>(r.last_day - day) * day_height;
		if (r.last_day < day) return r.top;
	}

	return {};
}

const explore_timeline_layout::row* explore_timeline_layout::year_row_at(const int y) const
{
	const auto* const r = row_at(y);
	if (!r || r->kind == row_kind::empty_years) return nullptr;

	for (auto i = r - rows.data(); i >= 0; --i)
	{
		if (rows[i].kind == row_kind::year) return &rows[i];
	}

	return nullptr;
}

const explore_timeline_layout::row* explore_timeline_layout::year_row(const int year) const
{
	const auto found = std::ranges::find_if(rows, [year](const row& r)
	{ return r.kind == row_kind::year && r.year == year; });
	return found == rows.end() ? nullptr : &*found;
}

const explore_timeline_layout::span* explore_timeline_layout::span_at(const std::vector<span>& spans, const int y)
{
	const auto found = std::ranges::upper_bound(spans, y, {}, &span::top);
	if (found == spans.begin()) return nullptr;
	const auto& s = *std::prev(found);
	return y < s.bottom ? &s : nullptr;
}

explore_timeline_layout layout_explore_timeline(const df::explore_model& model, const explore_timeline_metrics& metrics)
{
	using row = explore_timeline_layout::row;
	using kind = explore_timeline_layout::row_kind;

	explore_timeline_layout result;
	result.day_height = std::max(1, metrics.day_height);
	result.year_height = metrics.year_height;
	const auto day_height = result.day_height;
	auto y = metrics.top;

	// Years with nothing in them fold into one row, and so do months with nothing in them.
	auto empty_newest = 0;
	auto empty_oldest = 0;
	std::optional<row> days;
	std::optional<row> gap;

	const auto close_empty_years = [&]
	{
		if (empty_newest == 0) return;
		result.rows.push_back({kind::empty_years, y, y + metrics.gap_height, empty_newest, empty_oldest, 0, 0});
		y += metrics.gap_height;
		empty_newest = 0;
	};

	const auto close_days = [&]
	{
		if (!days) return;
		days->bottom = y;
		result.rows.push_back(*days);
		days.reset();
	};

	const auto close_gap = [&]
	{
		if (!gap) return;
		gap->top = y;
		gap->bottom = y + metrics.gap_height;
		y = gap->bottom;
		result.rows.push_back(*gap);
		gap.reset();
	};

	size_t month = 0;

	for (const auto& year : model.years)
	{
		const auto first_month = month;
		while (month < model.months.size() && model.months[month].year == year.year) ++month;

		if (year.counts.is_empty())
		{
			if (empty_newest == 0) empty_newest = year.year;
			empty_oldest = year.year;
			continue;
		}

		close_empty_years();
		result.rows.push_back(
			{kind::year, y, y + metrics.year_height, year.year, year.year, year.first_day, year.last_day});
		y += metrics.year_height;

		for (auto i = first_month; i < month; ++i)
		{
			const auto& m = model.months[i];

			if (m.counts.is_empty())
			{
				close_days();
				if (gap) gap->first_day = m.first_day;
				else gap = row{kind::gap, 0, 0, year.year, year.year, m.first_day, m.last_day};
				continue;
			}

			close_gap();
			if (days) days->first_day = m.first_day;
			else days = row{kind::days, y, 0, year.year, year.year, m.first_day, m.last_day};

			const auto height = static_cast<int>(m.last_day - m.first_day + 1) * day_height;
			result.months.push_back({static_cast<uint32_t>(i), y, y + height});
			y += height;
		}

		close_days();
		close_gap();
	}

	close_empty_years();
	result.height = y + metrics.bottom;

	// A week or a trip is drawn wherever its days are. Cut to its year, a week's days that are drawn are one
	// run; a trip that crosses a new year is drawn in two pieces.
	std::vector<const row*> day_rows;
	for (const auto& r : result.rows)
	{
		if (r.kind == kind::days) day_rows.push_back(&r);
	}

	const auto pieces = [&](const uint32_t first, const uint32_t last, const auto& add)
	{
		for (const auto* const r : day_rows)
		{
			const auto newest = std::min(last, r->last_day);
			const auto oldest = std::max(first, r->first_day);
			if (newest < oldest) continue;

			add(r->top + static_cast<int>(r->last_day - newest) * day_height,
			    r->top + static_cast<int>(r->last_day - oldest + 1) * day_height);
		}
	};

	for (uint32_t w = 0; w < model.weeks.size(); ++w)
	{
		auto top = std::numeric_limits<int>::max();
		auto bottom = std::numeric_limits<int>::min();

		pieces(model.weeks[w].first_day, model.weeks[w].last_day, [&](const int t, const int b)
		{
			top = std::min(top, t);
			bottom = std::max(bottom, b);
		});

		if (top < bottom) result.weeks.push_back({w, top, bottom});
	}

	for (uint32_t t = 0; t < model.trips.size(); ++t)
	{
		pieces(model.trips[t].first_day, model.trips[t].last_day,
		       [&](const int top, const int bottom) { result.trips.push_back({t, top, bottom}); });
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The view
///////////////////////////////////////////////////////////////////////////////////////////////////

explore_view::explore_view(view_state& state, view_host_ptr host) :
	_state(state), _host(std::move(host)), _tiles(state._async, [this]
{
	++_tile_revision;
	_host->frame()->invalidate(_globe_bounds);
}, explore_tile_capacity)
{
	_show_render_window = true;
	_scroller._moves_pixels = false;
	_scroller.changed_func = [this]
	{
		prepare_thumbnails();
		_state.invalidate_view(view_invalid::controller | view_invalid::tooltip);
	};
}

void explore_view::open()
{
	_state.open_in_history(view_type::explore);
}

void explore_view::show_item(const core::file_path path)
{
	const auto file = _state.item_index.find_item(path);
	if (!file.ft) return;
	const auto md = file.metadata.load();

	if (md && md->coordinate.is_valid())
	{
		stop_turn();
		_view = md->coordinate;
		_view_is_user_set = true;

		// About 400 km across the globe's half, as a place is seen among its neighbours.
		if (_globe_bounds.is_empty())
		{
			_zoom = 32.0;
		}
		else
		{
			const auto fit = std::max(1.0, globe_radius() / _zoom);
			const auto max_zoom = std::max(1.0, 256.0 * std::pow(2.0, globe_max_tile_level) / (2.0 * M_PI) / fit);
			_zoom = std::clamp(_globe_bounds.width() / 400.0 * 6371.0 / fit, 1.0, max_zoom);
		}

		_globe_invalid = true;
		_cluster_level_asked = -1;
		publish_globe_turn();
	}

	// The day a date search of the item's capture date lists it on, as the timeline counts it.
	if (const auto taken = df::date_term_keys_of(df::date_parts_prop::original, file, md.get()).original;
	    taken.is_valid())
	{
		_show_day = static_cast<uint32_t>(taken.to_days());
	}

	open();
	_state.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw);
}

void explore_view::collection_changed()
{
	_stale = true;
	if (_state.view_mode() != view_type::explore) return;
	_model_due = true;
	tick_model();
}

void explore_view::tick_model()
{
	// A run of weeks held near an end of the timeline goes on scrolling while the pointer rests.
	if (_choosing_weeks) choose_weeks_at(_weeks_loc);

	// Tiles that arrived while the globe had just been drawn are taken in once a tenth of a second has passed.
	if (_tiles_waiting && core::now_ms() - _globe_rendered_ms >= 100)
	{
		_tiles_waiting = false;
		_host->frame()->invalidate(_globe_bounds);
	}

	if (!_model_due || _building || core::now_ms() - _model_requested_ms < 1000) return;
	if (_state.view_mode() != view_type::explore)
	{
		// Left stale, and made again on showing.
		_model_due = false;
		return;
	}

	request_model();
}

void explore_view::activate(const sizei extent)
{
	_extent = extent;

	if (!_globe.is_ready())
	{
		files ff;
		_globe.set_source(globe_composite_water(ff.image_to_surface(load_app_resource(app_resource::map_png))));
	}

	if (_stale || (!_model && !_arrived)) request_model();
	follow_globe_turns();
	_globe_invalid = true;
	update_status();
}

void explore_view::follow_globe_turns()
{
	if (_globe_turns_seen == _state.globe_turns) return;
	_globe_turns_seen = _state.globe_turns;
	if (!_state.globe_facing.is_valid()) return;

	// The sidebar's globe expanded: its face, all of it in sight.
	stop_turn();
	_view = _state.globe_facing;
	_zoom = 1.0;
	_view_is_user_set = true;
	_globe_invalid = true;
}

void explore_view::publish_globe_turn()
{
	_state.turn_globes(_view);
	_globe_turns_seen = _state.globe_turns;
}

// What is held for the screen is let go, and found again on showing: tiles from the store, pictures from
// the index, the model from the collection if it has changed meanwhile.
void explore_view::deactivate()
{
	stop_turn();
	_tiles.clear();
	_globe_surface.reset();
	_globe_texture.reset();
	_thumbnails.clear();
	_hover = {};
	_pressed = {};
	_choosing_weeks = false;
	_grabbed = {};
	clear_marks();
}

void explore_view::refresh()
{
	request_model();
}

void explore_view::request_model()
{
	if (_building)
	{
		_build_again = true;
		return;
	}

	_building = true;
	_stale = false;
	_model_due = false;
	_model_requested_ms = core::now_ms();

	df::explore_request request;
	request.today = platform::utc_to_local(core::utc_now()).to_days();
	request.start_year = setting.sidebar.history_start_year;

	const auto generation = ++_model_generation;

	// The index is read on the query queue and the gazetteer on the location worker; the view is reached
	// again only back on the UI thread.
	_state.queue_async(async_queue::query, [&state = _state, weak = weak_from_this(), request, generation]() mutable
	{
		request.samples = state.item_index.explore_samples();

		state.queue_location([&state, weak, request = std::move(request), generation](location_cache& locations) mutable
		{
			auto model = std::make_shared<const df::explore_model>(
				df::build_explore_model(std::move(request), &locations));

			state.queue_ui([weak, model = std::move(model), generation]() mutable
			{
				if (const auto self = weak.lock()) self->model_built(std::move(model), generation);
			});
		});
	});
}

void explore_view::model_built(df::explore_model_ptr model, const uint32_t generation)
{
	_building = false;

	if (generation == _model_generation)
	{
		// Taken up when the timeline is next laid out: now, if Explore is on screen, else when it is shown.
		_arrived = std::move(model);
		if (_state.view_mode() == view_type::explore) _state.invalidate_view(view_invalid::view_layout);
	}

	if (std::exchange(_build_again, false)) request_model();
	else tick_model();
}

// The model that arrived replaces what is shown, and whatever pointed into the old one is let go. Not while the user
// turns the globe or drags across weeks: it is taken up as they let go.
void explore_view::adopt_arrived_model()
{
	if (!_arrived || _turning || _choosing_weeks || !_pressed.is_none()) return;

	_model = std::move(_arrived);
	_hover = {};
	_pressed = {};
	_choosing_weeks = false;
	clear_marks();
	frame_on_collection();
	_cluster_level_asked = -1;
	update_status();
	_state.invalidate_view(view_invalid::command_state | view_invalid::app_layout | view_invalid::tooltip);
}

void explore_view::update_status()
{
	if (!_model)
	{
		_status = tt.explore_reading;
		return;
	}

	if (_model->is_empty())
	{
		_status = tt.explore_empty;
		return;
	}

	_status = format_plural_text(tt.title_item_count_fmt, _model->total.total());

	if (_model->first_year != 0)
	{
		_status += " \xC2\xB7 ";
		_status += format_years(_model->last_year, _model->first_year);
	}

	if (!_model->trips.empty())
	{
		_status += " \xC2\xB7 ";
		_status += format_plural_text(tt.explore_trips_fmt, static_cast<int64_t>(_model->trips.size()));
	}
}

std::string_view explore_view::title()
{
	_title = std::format("{}: {}", core::app_name(), tt.command_explore);
	return _title;
}

std::string_view explore_view::status()
{
	return _status;
}

view_bars explore_view::bars() const
{
	auto result = history_view_bars({});
	result.bottom_leading = {commands::view_zoom_out, commands::view_zoom_in};
	return result;
}

void explore_view::exit()
{
	_state.view_mode(view_type::items);
}

void explore_view::display_changed()
{
	// Place names are the language's, so they are asked for again.
	_cluster_areas.clear();
	_cluster_areas_asked.clear();
}

void explore_view::broadcast_event(const view_element_event& event) const
{
	if (_timeline_info) _timeline_info->dispatch_event(event);
	if (event.type == view_element_event_type::free_graphics_resources)
	{
		_globe_texture.reset();

		for (const auto& [path, t] : _thumbnails)
		{
			t.texture.reset();
			t.uploaded.reset();
		}
	}
}

void explore_view::open_search(const df::search_t& search)
{
	// Explore changes the query only: grouping, sorting and filters stay the user's, as from the sidebar.
	if (_state.open(_host, search, {})) _state.view_mode(view_type::items);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The globe
///////////////////////////////////////////////////////////////////////////////////////////////////

double explore_view::globe_radius() const
{
	const auto margin = core::round(28 * _scale_factor);
	const auto fit = std::max(16, std::min(_globe_bounds.width(), _globe_bounds.height()) / 2 - margin);
	return fit * _zoom;
}

globe_projection explore_view::projection() const
{
	return {_view, _globe_bounds.center(), globe_radius()};
}

// The collection deserves to be seen first from the middle of where it was taken.
void explore_view::frame_on_collection()
{
	if (_view_is_user_set || !_model) return;

	globe_framer framer;
	for (const auto& located : _model->located) framer.add(located.coordinate, 1.0);

	if (const auto framed = framer.view(); framed.is_valid())
	{
		_view = framed;
		_globe_invalid = true;
	}
}

// When what the pointer rests on was all taken out of sight, the globe turns to where it was taken. Not when
// zoomed in close: there a turn half way round the world would be a leap, not a turn.
void explore_view::show_marked_places()
{
	if (_cluster_marks.empty() || !_clusters || _cluster_vectors.size() != _cluster_marks.size() || _turning ||
	    _zoom > 4.0)
		return;

	const auto sphere = projection();
	globe_framer framer;
	auto marked = false;

	for (size_t i = 0; i < _cluster_marks.size(); ++i)
	{
		if (_cluster_marks[i] == 0) continue;
		marked = true;

		if (const auto at = sphere.project_world(_cluster_vectors[i]); at && _globe_bounds.contains(*at)) return;
		framer.add(_clusters->clusters[i].centre, _cluster_marks[i]);
	}

	const auto target = framer.view();
	if (!marked || !target.is_valid() || target == _turn_to) return;

	_view_is_user_set = true;

	if (!render::animations_enabled)
	{
		_view = target;
		_globe_invalid = true;
		_host->frame()->invalidate(_globe_bounds);
		return;
	}

	_turn_from = _view;
	_turn_to = target;
	_turn_start_ms = core::now_ms();
	ui::animations[this] = [weak = weak_from_this()]
	{
		const auto self = weak.lock();
		return self && self->step_turn();
	};
	_host->invalidate(ui::host_invalid::animations);
}

// Along the great circle between where the globe faced and where it turns to, easing in and out.
bool explore_view::step_turn()
{
	if (!_turn_to.is_valid()) return false;

	constexpr auto duration_ms = 650.0;
	const auto t = std::clamp(static_cast<double>(core::now_ms() - _turn_start_ms) / duration_ms, 0.0, 1.0);
	const auto eased = t * t * (3.0 - 2.0 * t);

	const auto from = globe_world_vector(_turn_from);
	const auto to = globe_world_vector(_turn_to);
	const auto cosine = std::clamp(from[0] * to[0] + from[1] * to[1] + from[2] * to[2], -1.0, 1.0);
	const auto angle = std::acos(cosine);
	auto along = to;

	if (angle > 1e-6)
	{
		const auto a = std::sin((1.0 - eased) * angle) / std::sin(angle);
		const auto b = std::sin(eased * angle) / std::sin(angle);
		for (auto i = 0; i < 3; ++i) along[i] = a * from[i] + b * to[i];
	}

	_view = gps_coordinate(gps_coordinate::rad2deg(std::asin(std::clamp(along[1], -1.0, 1.0))),
	                       globe_safe_longitude(gps_coordinate::rad2deg(std::atan2(along[0], along[2]))));
	_globe_invalid = true;
	_host->frame()->invalidate(_globe_bounds);

	if (t < 1.0) return true;
	_turn_to = {};
	return false;
}

void explore_view::stop_turn()
{
	ui::animations.erase(this);
	_turn_to = {};
}

void explore_view::step_globe_zoom(const int direction)
{
	zoom_globe(std::pow(explore_zoom_per_notch, 2 * direction), {});
}

void explore_view::zoom_globe(const double factor, const std::optional<pointi> about)
{
	stop_turn();

	const auto before = projection();
	const auto anchor = about ? before.unproject(*about) : std::optional<gps_coordinate>{};
	const auto fit = std::max(1.0, before.radius() / _zoom);
	const auto max_zoom = std::max(1.0, 256.0 * std::pow(2.0, globe_max_tile_level) / (2.0 * M_PI) / fit);
	const auto zoom = std::clamp(_zoom * factor, 1.0, max_zoom);

	if (zoom == _zoom) return;
	_zoom = zoom;

	// The point under the pointer stays under it.
	if (anchor && about)
	{
		const auto after = projection();
		const auto unit_x = (about->x - after.center().x) / after.radius();
		const auto unit_y = (after.center().y - about->y) / after.radius();
		if (const auto placed = globe_view_placing(*anchor, unit_x, unit_y, _view))
		{
			_view = *placed;
			publish_globe_turn();
		}
	}

	_view_is_user_set = true;
	_globe_invalid = true;
	request_clusters();
	_host->frame()->invalidate(_globe_bounds);
}

void explore_view::request_clusters()
{
	if (!_model) return;

	// Cells are measured in the pixels markers are drawn in, which grow with the display's scale.
	const auto level = df::explore_cluster_level(globe_radius() / std::max(0.25, _scale_factor));
	if (level == _cluster_level_asked) return;
	_cluster_level_asked = level;

	const auto generation = ++_cluster_generation;

	_state.queue_async(async_queue::query, [&state = _state, weak = weak_from_this(), model = _model, level, generation]
	{
		auto clusters = std::make_shared<const df::explore_clusters>(df::cluster_explore_model(*model, level));

		state.queue_ui([weak, model, clusters = std::move(clusters), generation]() mutable
		{
			const auto self = weak.lock();
			if (!self || generation != self->_cluster_generation || model != self->_model) return;

			self->_clusters = std::move(clusters);
			self->_clusters_of = model;
			self->_cluster_vectors.clear();
			self->_cluster_vectors.reserve(self->_clusters->clusters.size());
			for (const auto& c : self->_clusters->clusters)
				self->_cluster_vectors.push_back(globe_world_vector(c.centre));
			self->_cluster_areas.clear();
			self->_cluster_areas_asked.clear();
			self->_hover = {};
			self->clear_marks();
			self->_host->frame()->invalidate();
			self->_state.invalidate_view(view_invalid::tooltip);
		});
	});
}

map_location_area explore_view::cluster_area(const uint32_t cluster) const
{
	if (const auto found = _cluster_areas.find(cluster); found != _cluster_areas.end()) return found->second;

	const auto& c = _clusters->clusters[cluster];
	map_location_area area;
	area.count = c.count;
	area.position = c.centre;
	area.min_latitude = c.min_latitude;
	area.min_longitude = c.min_longitude;
	area.max_latitude = c.max_latitude;
	area.max_longitude = c.max_longitude;
	return area;
}

// Naming a place reads the gazetteer, so the answer arrives later and is kept for the clusters it was
// asked for; a new set of clusters forgets it.
void explore_view::resolve_cluster(const uint32_t cluster)
{
	if (!_clusters || cluster >= _clusters->clusters.size()) return;
	if (!_cluster_areas_asked.insert(cluster).second) return;

	_state.queue_location([&state = _state, weak = weak_from_this(), clusters = _clusters, cluster,
	                       request = cluster_area(cluster)](const location_cache& locations)
	{
		const auto selected = name_map_area(locations, request);

		auto resolved = request;
		resolved.name = selected.id == 0 ? std::string(tt.location_remote.sv()) : qualified_name(selected);
		resolved.state = selected.state;
		resolved.country = selected.country;
		resolved.population = selected.population;
		resolved.place_position = selected.id == 0 ? gps_coordinate{} : selected.position;

		state.queue_ui([weak, clusters, cluster, resolved = std::move(resolved)]() mutable
		{
			const auto self = weak.lock();
			if (!self || clusters != self->_clusters) return;

			self->_cluster_areas[cluster] = std::move(resolved);
			self->_state.invalidate_view(view_invalid::tooltip);
		});
	});
}

explore_target explore_view::globe_target_at(const pointi loc) const
{
	if (!_clusters || _cluster_vectors.size() != _clusters->clusters.size() || !_globe_bounds.contains(loc)) return {};

	const auto sphere = projection();
	const auto& clusters = _clusters->clusters;

	// Last drawn is on top, so it is the one hit.
	for (auto i = clusters.size(); i-- > 0;)
	{
		const auto at = sphere.project_world(_cluster_vectors[i]);
		if (!at) continue;

		const auto r = marker_radius(clusters[i].count, _scale_factor);
		const auto dx = static_cast<int64_t>(at->x - loc.x);
		const auto dy = static_cast<int64_t>(at->y - loc.y);
		if (dx * dx + dy * dy <= static_cast<int64_t>(r) * r)
			return {explore_target_kind::cluster, static_cast<uint32_t>(i)};
	}

	return {};
}

void explore_view::render_globe(render::draw_context& dc)
{
	const auto bounds = _globe_bounds;
	if (bounds.is_empty()) return;

	const render::scoped_clip clip(dc, bounds);
	dc.draw_rect(bounds, core::color(core::darken(ui::style::colors.view_background, 0.45f), dc.colors.alpha));

	const auto sphere = projection();
	const auto radius = sphere.radius();
	const auto centre = sphere.center();

	// A soft glow round the whole globe, gone once the globe is larger than the window.
	if (radius < bounds.width() + bounds.height())
	{
		const auto glow = core::color(0.35f, 0.6f, 1.0f, 0.05f * dc.colors.alpha);
		for (auto i = 6; i >= 1; --i)
		{
			const auto r = core::round(radius + i * 3.0 * _scale_factor);
			dc.fill_ellipse(recti(centre.x - r, centre.y - r, centre.x + r, centre.y + r), glow);
		}
	}

	const auto extent = bounds.extent();
	const globe_key key{_view, radius, extent, _tile_revision};

	// Only new tiles, drawn again a moment ago: the globe as drawn stays up until the tick that takes them in.
	const auto tiles_only = !_globe_invalid && _globe_texture && key.view == _rendered.view &&
	                        key.radius == _rendered.radius && key.extent == _rendered.extent &&
	                        key.tile_revision != _rendered.tile_revision;
	_tiles_waiting = tiles_only && core::now_ms() - _globe_rendered_ms < 100;

	if (!_tiles_waiting && (_globe_invalid || !(key == _rendered) || !_globe_texture))
	{
		if (!_globe_surface || _globe_surface->dimensions() != extent)
		{
			_globe_surface = std::make_shared<core::surface>();

			// A failed alloc still reports the requested extent, so the buffer is dropped rather than kept to
			// be taken for the right size next frame.
			if (!_globe_surface->alloc(extent, core::texture_format::ARGB)) _globe_surface.reset();
		}

		if (!_globe_texture) _globe_texture = dc.create_texture();

		if (_globe_surface && _globe_texture)
		{
			const auto level = std::clamp(static_cast<int>(std::ceil(globe_tile_level(radius))), 0,
			                              globe_max_tile_level);
			explore_tile_source tiles(_tiles, level);
			const globe_projection local(_view, pointi(centre.x - bounds.left, centre.y - bounds.top), radius);

			if (_globe.render(*_globe_surface, local, &tiles))
			{
				_globe_texture->update(_globe_surface);
				_rendered = key;
				_globe_invalid = false;
				_tiles_drawn = tiles.drawn.load(std::memory_order_relaxed);
				_globe_rendered_ms = core::now_ms();
			}

			order_tiles_from(tiles.wanted, _view);
			_tiles.show(tiles.wanted);
		}
	}

	if (_globe_texture && _globe_surface)
	{
		dc.draw_texture(_globe_texture, recti(bounds.top_left(), extent), dc.colors.alpha);
	}

	render_clusters(dc, sphere);

	const auto text = core::color(ui::style::colors.view_text, 0.55f * dc.colors.alpha);
	const auto pad = core::round(8 * _scale_factor);

	// The hint is for the globe as it opens; zoomed in, it would sit over the streets.
	if (_model && !_model->is_empty() && _zoom < 1.3)
	{
		const recti hint(bounds.left + pad, bounds.top + pad, bounds.right - pad, bounds.top + pad + _line_height);
		dc.draw_text(tt.explore_hint, hint, render::font_face::dialog, render::text_style::single_line, text, {});
	}

	// The OSM tile usage policy requires attribution wherever its tiles are shown.
	if (_tiles_drawn) draw_map_attribution(dc, bounds);
}

void explore_view::render_clusters(render::draw_context& dc, const globe_projection& sphere) const
{
	if (!_clusters || _cluster_vectors.size() != _clusters->clusters.size()) return;

	const auto& clusters = _clusters->clusters;
	const auto marking = !_cluster_marks.empty();
	const auto accent = ui::style::colors.important_background;
	const auto hovered = _hover.kind == explore_target_kind::cluster ? static_cast<int>(_hover.index) : -1;
	const auto visible = _globe_bounds.inflate(core::round(32 * _scale_factor));
	const auto ring = std::max(1.0, 1.5 * _scale_factor);

	for (size_t i = 0; i < clusters.size(); ++i)
	{
		double depth = 0.0;
		const auto at = sphere.project_world(_cluster_vectors[i], &depth);
		if (!at || !visible.contains(*at)) continue;

		const auto marked = marking ? _cluster_marks[i] : 0u;
		const auto r = marker_radius(marking && marked ? marked : clusters[i].count, _scale_factor);
		const recti circle(at->x - r, at->y - r, at->x + r, at->y + r);

		// Toward the limb a marker is seen edge on, so it fades rather than crowding the horizon.
		const auto limb = static_cast<float>(std::clamp((depth - globe_min_visible_z) / 0.25, 0.0, 1.0));
		const auto alpha = dc.colors.alpha * limb * (marking && marked == 0 ? 0.28f : 1.0f);
		const auto fill = marking && marked ? explore_mark_color : core::color(accent, 1.0f);

		dc.fill_ellipse(circle, core::color(fill.r, fill.g, fill.b, 0.72f * alpha));
		dc.draw_ellipse(circle, core::color(1.0f, 1.0f, 1.0f, 0.55f * alpha), ring);

		if (r >= core::round(10 * _scale_factor))
		{
			const auto count = marking && marked ? marked : clusters[i].count;
			dc.draw_text(abbreviated_count(count), circle, render::font_face::dialog,
			             render::text_style::single_line_center, core::color(1.0f, 1.0f, 1.0f, 0.95f * alpha), {});
		}

		if (static_cast<int>(i) == hovered)
		{
			const auto outer = circle.inflate(core::round(3 * _scale_factor));
			dc.draw_ellipse(outer, core::color(1.0f, 1.0f, 1.0f, 0.9f * dc.colors.alpha), ring * 1.6);
		}
	}
}

void explore_view::bubble_cluster(view_hover_element& hover, const uint32_t cluster)
{
	if (!_clusters || cluster >= _clusters->clusters.size() || cluster >= _cluster_vectors.size()) return;

	resolve_cluster(cluster);

	const auto& c = _clusters->clusters[cluster];
	const auto area = cluster_area(cluster);
	const auto named = !area.name.empty() && area.place_position.is_valid();

	hover.elements->add(make_icon_element(icon_index::location, flex_item::no_break));
	hover.elements->add(
		std::make_shared<text_element>(named ? area.name : format_plural_text(tt.map_items_here_fmt, c.count),
	                                   render::font_face::title, render::text_style::multiline, flex_item::line_break));

	add_bubble_thumbnail(hover, c.representative.path);

	if (named)
	{
		hover.elements->add(std::make_shared<text_element>(str_format(tt.click_items_from_fmt.sv(), c.count, area.name),
		                                                   render::font_face::dialog, render::text_style::multiline,
		                                                   flex_item::new_line));
	}

	if (!_marked_days.empty())
	{
		const auto first = df::explore_date(_marked_span.first).year;
		const auto last = df::explore_date(_marked_span.second).year;
		const auto taken = first == last ? str_format(tt.explore_taken_in_fmt.sv(), first)
		                                 : str_format(tt.explore_taken_fmt.sv(), first, last);
		hover.elements->add(std::make_shared<text_element>(taken, render::font_face::dialog,
		                                                   render::text_style::multiline, flex_item::new_line));
	}

	// The action names the search a click runs - a place and a radius, or where nothing names the place, its
	// coordinate and one - as the sidebar globe's does, so the count after the click is never a surprise.
	hover.elements->add(std::make_shared<action_element>(str_format(tt.search_within_fmt.sv(),
	                                                                format_distance_km(area.search_radius_km()),
	                                                                named ? area.name : area.position.str())));

	const auto sphere = projection();
	if (const auto at = sphere.project_world(_cluster_vectors[cluster]))
	{
		const auto r = marker_radius(c.count, _scale_factor);
		hover.window_bounds = hover.active_bounds = recti(at->x - r, at->y - r, at->x + r, at->y + r);
	}

	hover.horizontal = false;
	hover.preferred_size = 220;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The timeline
///////////////////////////////////////////////////////////////////////////////////////////////////

pointi explore_view::timeline_offset() const
{
	return {_timeline_content.left, _timeline_content.top - _scroller.scroll_offset().y};
}

void explore_view::layout(render::measure_context& mc, const sizei extent)
{
	adopt_arrived_model();

	_extent = extent;
	_scale_factor = mc.scale_factor;
	_line_height = mc.text_line_height(render::font_face::dialog);

	const auto min_timeline = core::round(320 * mc.scale_factor);
	const auto timeline_width = std::min(extent.cx, std::clamp(extent.cx * 2 / 5, min_timeline,
	                                                           core::round(560 * mc.scale_factor)));

	_timeline_bounds = recti(extent.cx - timeline_width, 0, extent.cx, extent.cy);
	_globe_bounds = recti(0, 0, _timeline_bounds.left, extent.cy);

	layout_timeline(mc);
	request_clusters();
}

void explore_view::layout_timeline(render::measure_context& mc)
{
	// The timeline stands at a day rather than at a height, so a new model or a new size finds it at the time it was
	// left - as closing Explore and opening it again, or coming back to it, shows. At the top it stays at the top,
	// where today is.
	std::optional<std::pair<uint32_t, int>> standing;
	if (const auto scrolled = _scroller.scroll_offset().y; scrolled > 0 && !_layout.rows.empty())
	{
		if (const auto day = _layout.day_near(scrolled + _layout.year_height))
		{
			if (const auto top = _layout.top_of(*day)) standing = std::pair{*day, *top - scrolled};
		}
	}

	const auto scale = mc.scale_factor;
	const auto scroll_width = mc.scroll_width;
	const auto title_height = mc.text_line_height(render::font_face::title);

	_pad = core::round(10 * scale);

	// The scroll bar is a rail of the years, so it is as wide as a year's number.
	const auto widest_year = core::to_string(_model ? _model->last_year : 2000);
	_rail_width = std::max(scroll_width, mc.measure_text(widest_year, render::font_face::dialog,
	                                                     render::text_style::single_line, 1000)
	                                             .cx +
	                                         mc.padding2 * 2);
	_timeline_content = _timeline_bounds;
	_timeline_content.right -= _rail_width;
	const auto width = _timeline_content.width();
	if (!_timeline_info)
		_timeline_info = std::make_shared<view_info_element>(icon_index::time, tt.timeline_title,
		                                                     tt.explore_timeline_hint);
	const auto info_width = std::max(1, width - 2 * _pad);
	const auto info_height = _timeline_info->measure(mc, info_width).cy;
	const auto show_info = info_height + 2 * _pad < _timeline_bounds.height() / 3;
	_timeline_info->is_visible(show_info);
	if (show_info) _timeline_info->layout(mc, recti(pointi(_pad, _pad), sizei(info_width, info_height)));

	_gutter_width = core::round(104 * scale);
	_bars_left = _pad + _gutter_width + _pad;
	_bars_width = std::clamp((width - _bars_left) * 2 / 5, core::round(72 * scale), core::round(200 * scale));
	_lane_left = _bars_left + _bars_width + core::round(16 * scale);
	_thumbnail_height = core::round(70 * scale);

	explore_timeline_metrics metrics;
	metrics.day_height = std::max(2, core::round(3 * scale));
	metrics.year_height = std::max(title_height, _line_height + core::round(8 * scale)) + 2 * _pad;
	metrics.gap_height = _line_height + core::round(10 * scale);
	metrics.top = (show_info ? _timeline_info->bounds.bottom + _pad : 0) + core::round(4 * scale);
	metrics.bottom = core::round(24 * scale);

	_layout = _model ? layout_explore_timeline(*_model, metrics) : explore_timeline_layout{};
	_busiest_day = 1;
	_year_chips.clear();
	_trip_labels.clear();

	if (_model)
	{
		for (const auto& day : _model->days) _busiest_day = std::max(_busiest_day, day.total());

		// Each year's places, as chips after its count in its header, as many as fit.
		const auto chip_height = _line_height + core::round(6 * scale);
		const auto chip_pad = core::round(8 * scale);

		for (uint32_t i = 0; i < _model->year_places.size(); ++i)
		{
			const auto& place = _model->year_places[i];
			const auto year = std::ranges::find(_model->years, place.year, &df::explore_year::year);
			if (year == _model->years.end()) continue;
			auto& chips = _year_chips[place.year];

			auto x = _pad;

			if (chips.empty())
			{
				x += mc.measure_text(core::to_string(place.year), render::font_face::title,
				                     render::text_style::single_line, width)
				         .cx;
				x += chip_pad;
				x += mc.measure_text(core::format_count(year->counts.total()), render::font_face::dialog,
				                     render::text_style::single_line, width)
				         .cx;
				x += chip_pad * 2;
			}
			else
			{
				x = chips.back().bounds.right + chip_pad / 2;
			}

			const auto text_width =
				mc.measure_text(place.place.name, render::font_face::dialog, render::text_style::single_line, width).cx;
			const recti chip(x, (metrics.year_height - chip_height) / 2, x + text_width + chip_pad * 2,
			                 (metrics.year_height + chip_height) / 2);

			if (chip.right <= width - _pad) chips.push_back({i, chip});
		}

		// Trip labels run down the lane, each beside its trip where there is room and pushed down past the
		// one before where there is not, so none covers another.
		const auto label_height = _line_height * 2 + core::round(4 * scale);
		auto next_free = std::numeric_limits<int>::min();
		uint32_t last_trip = std::numeric_limits<uint32_t>::max();

		for (const auto& piece : _layout.trips)
		{
			if (piece.index == last_trip) continue;
			last_trip = piece.index;

			const auto middle = (piece.top + piece.bottom) / 2;
			const auto top = std::max(middle - label_height / 2, next_free);
			const recti bounds(_lane_left + core::round(12 * scale), top, width - _pad, top + label_height);
			_trip_labels.push_back({piece.index, bounds, middle});
			next_free = bounds.bottom + core::round(4 * scale);
		}
	}

	const recti scroll_bounds(_timeline_content.right, _timeline_content.top, _timeline_bounds.right,
	                          _timeline_bounds.bottom);
	_scroller.layout({width, std::max(_layout.height, _trip_labels.empty() ? 0 : _trip_labels.back().bounds.bottom)},
	                 _timeline_content, scroll_bounds);

	if (standing)
	{
		if (const auto top = _layout.top_of_or_older(standing->first))
			_scroller.scroll_offset(_host, 0, *top - standing->second);
	}

	// A week an item was shown from: in the middle of the timeline, marked as the keyboard marks a week.
	if (_show_day && _model)
	{
		const auto day = *std::exchange(_show_day, std::nullopt);

		for (uint32_t i = 0; i < _layout.weeks.size(); ++i)
		{
			const auto& span = _layout.weeks[i];
			const auto& week = _model->weeks[span.index];
			if (day < week.first_day || day > week.last_day) continue;

			_keyboard_week = i;
			_keyboard_used = true;
			_scroller.scroll_offset(_host, 0, (span.top + span.bottom - _timeline_content.height()) / 2);
			break;
		}
	}

	// A section names the band of the rail that ends where it says, so each year's runs to where the next begins.
	std::vector<view_scroller_section> sections;
	for (const auto& r : _layout.rows)
	{
		if (r.kind != explore_timeline_layout::row_kind::year) continue;
		if (!sections.empty()) sections.back().y = r.top;
		sections.push_back({core::to_string(r.year), {}, _layout.height});
	}
	_scroller.sections(std::move(sections));

	prepare_thumbnails();
}

// The pictures the timeline is about to show are asked for now, outside drawing, so drawing only reads
// what has arrived. Those far from the screen are let go once many are held, so a long scroll down the years holds
// only what is near.
void explore_view::prepare_thumbnails()
{
	if (!_model) return;

	const auto view_top = _scroller.scroll_offset().y - _timeline_content.height();
	const auto view_bottom = _scroller.scroll_offset().y + _timeline_content.height() * 2;
	core::unique_paths wanted;

	const auto want = [this, &wanted](const core::file_path& path)
	{
		if (path.is_empty()) return;
		wanted.emplace(path);
		auto& t = _thumbnails[path];

		if (!t.item)
		{
			const auto indexed = _state.item_index.find_item(path);
			if (!indexed.ft) return;
			t.item = std::make_shared<df::item_element>(path, indexed);
		}

		if (is_valid(t.item->thumbnail_surface())) return;

		if (is_valid(t.item->thumbnail()))
		{
			t.item->stage_thumbnail_surface(_state._async, true);
		}
		else if (!t.requested)
		{
			t.requested = true;
			_state.item_index.queue_load_thumbnail(t.item, view_invalid::view_redraw);
		}
	};

	for (const auto& span : _layout.months)
	{
		if (span.bottom >= view_top && span.top <= view_bottom) want(_model->months[span.index].representative.path);
	}

	for (const auto& label : _trip_labels)
	{
		if (label.bounds.bottom >= view_top && label.bounds.top <= view_bottom)
			want(_model->trips[label.trip].representative.path);
	}

	if (_thumbnails.size() > wanted.size() + max_spare_thumbnails)
		std::erase_if(_thumbnails, [&wanted](const auto& entry) { return !wanted.contains(entry.first); });
}

void explore_view::render_thumbnail(render::draw_context& dc, const core::file_path& path, const recti bounds,
                                    const float radius) const
{
	const auto found = _thumbnails.find(path);
	const auto placeholder = core::color(ui::style::colors.view_text, 0.08f * dc.colors.alpha);

	if (found == _thumbnails.end() || !found->second.item)
	{
		dc.draw_rounded_rect(bounds, placeholder, core::round(radius));
		return;
	}

	const auto& t = found->second;
	const auto surface = t.item->thumbnail_surface();

	if (!is_valid(surface))
	{
		// Arrived since it was asked for, and needs only decoding.
		if (is_valid(t.item->thumbnail())) t.item->stage_thumbnail_surface(_state._async, true);
		dc.draw_rounded_rect(bounds, placeholder, core::round(radius));
		return;
	}

	if (!t.texture) t.texture = dc.create_texture();
	if (!t.texture) return;

	if (t.uploaded != surface)
	{
		if (t.texture->update(surface) == render::texture_update_result::failed) return;
		t.uploaded = surface;
	}

	// Cropped to fill, in the picture's own axes, then turned as the user's settings show it.
	const auto orientation = t.item->layout_orientation();
	const auto turned = setting.show_rotated && orientation != core::orientation::top_left;
	const auto flips = turned && flips_xy(orientation);
	const auto dims = surface->dimensions();
	const auto want_cx = flips ? bounds.height() : bounds.width();
	const auto want_cy = flips ? bounds.width() : bounds.height();

	recti source;

	if (static_cast<int64_t>(dims.cx) * want_cy > static_cast<int64_t>(dims.cy) * want_cx)
	{
		const auto cx = core::mul_div(dims.cy, want_cx, std::max(1, want_cy));
		source = recti((dims.cx - cx) / 2, 0, (dims.cx + cx) / 2, dims.cy);
	}
	else
	{
		const auto cy = core::mul_div(dims.cx, want_cy, std::max(1, want_cx));
		source = recti(0, (dims.cy - cy) / 2, dims.cx, (dims.cy + cy) / 2);
	}

	if (turned)
	{
		dc.draw_texture(t.texture, quadd(bounds).transform(to_simple_transform(orientation)), source, dc.colors.alpha,
		                render::texture_sampler::bilinear);
	}
	else
	{
		dc.draw_texture(t.texture, bounds, source, dc.colors.alpha, render::texture_sampler::bilinear, radius);
	}
}

recti explore_view::week_bounds(const uint32_t week) const
{
	const auto found = std::ranges::find(_layout.weeks, week, &explore_timeline_layout::span::index);
	if (found == _layout.weeks.end()) return {};

	const auto offset = timeline_offset();
	return recti(_bars_left - core::round(4 * _scale_factor), found->top, _lane_left - core::round(6 * _scale_factor),
	             found->bottom)
	    .offset(offset);
}

// The rows a run of weeks is drawn over, in content coordinates: from the top of the newest to the bottom of
// the oldest. A week cut by a folded month is drawn only where its days are, so where it is drawn is the
// layout's to say, not its dates'.
std::optional<std::pair<int, int>> explore_view::weeks_extent(const uint32_t newer, const uint32_t older) const
{
	const auto newest = std::ranges::find(_layout.weeks, newer, &explore_timeline_layout::span::index);
	const auto oldest = std::ranges::find(_layout.weeks, older, &explore_timeline_layout::span::index);
	if (newest == _layout.weeks.end() || oldest == _layout.weeks.end()) return {};
	return std::make_pair(newest->top, oldest->bottom);
}

void explore_view::scroll_to_week(const uint32_t week)
{
	const auto found = std::ranges::find(_layout.weeks, week, &explore_timeline_layout::span::index);
	if (found == _layout.weeks.end()) return;

	const auto top = _scroller.scroll_offset().y;
	const auto height = _timeline_content.height();
	const auto header = _layout.year_height;

	if (found->top - header < top) _scroller.scroll_offset(_host, 0, found->top - header);
	else if (found->bottom > top + height) _scroller.scroll_offset(_host, 0, found->bottom - height);
}

explore_target explore_view::timeline_target_at(const pointi loc) const
{
	if (!_model || !_timeline_content.contains(loc)) return {};

	using kind = explore_timeline_layout::row_kind;
	const auto offset = timeline_offset();
	const auto x = loc.x - offset.x;
	auto y = loc.y - offset.y;
	const auto* r = _layout.row_at(y);
	auto under_sticky_header = false;

	// The header of the year scrolled under the top stays there, over everything else, and answers for what
	// is under it.
	if (loc.y < _timeline_content.top + _layout.year_height)
	{
		const auto scroll_top = _scroller.scroll_offset().y;

		if (const auto* sticky = _layout.year_row_at(scroll_top); sticky && sticky->top < scroll_top)
		{
			r = sticky;
			y = sticky->top + (loc.y - _timeline_content.top);
			under_sticky_header = true;
		}
	}

	// A trip's label is drawn over whatever row it hangs over, a header or a folded gap among them.
	if (!under_sticky_header && x >= _lane_left - core::round(6 * _scale_factor))
	{
		for (const auto& label : _trip_labels)
		{
			if (label.bounds.contains({x, y})) return {explore_target_kind::trip, label.trip};
		}
	}

	if (!r || y >= r->bottom) return {};

	if (r->kind == kind::year)
	{
		const auto year = std::ranges::find(_model->years, r->year, &df::explore_year::year);
		if (year == _model->years.end()) return {};

		if (const auto chips = _year_chips.find(r->year); chips != _year_chips.end())
		{
			for (const auto& chip : chips->second)
			{
				if (chip.bounds.contains({x, y - r->top})) return {explore_target_kind::year_place, chip.year_place};
			}
		}

		return {explore_target_kind::year, static_cast<uint32_t>(year - _model->years.begin())};
	}

	if (r->kind != kind::days) return {};

	if (x >= _lane_left - core::round(6 * _scale_factor))
	{
		if (const auto* piece = explore_timeline_layout::span_at(_layout.trips, y);
		    piece && x < _lane_left + core::round(14 * _scale_factor))
		{
			return {explore_target_kind::trip, piece->index};
		}

		return {};
	}

	if (x < _bars_left - _pad / 2)
	{
		if (const auto* month = explore_timeline_layout::span_at(_layout.months, y))
			return {explore_target_kind::month, month->index};
		return {};
	}

	if (const auto* week = explore_timeline_layout::span_at(_layout.weeks, y))
	{
		return {explore_target_kind::week, week->index};
	}

	return {};
}

void explore_view::render_year_header(render::draw_context& dc, const explore_timeline_layout::row& row,
                                      const int top) const
{
	const auto height = row.bottom - row.top;
	const recti band(_timeline_content.left, top, _timeline_content.right, top + height);
	const auto hovered_year = _hover.kind == explore_target_kind::year && _model->years[_hover.index].year == row.year;

	// Shaded as a group's header in Items is, and opaque, as the header held at the top covers what scrolls under it.
	dc.draw_rect(band, core::color(dc.colors.background, dc.colors.alpha));
	dc.draw_rounded_rect(band, core::color(0x000000, 0.222f * dc.colors.alpha), dc.padding1);
	if (hovered_year) dc.draw_rect(band, core::color(ui::style::colors.important_background, 0.18f * dc.colors.alpha));

	const auto text = core::color(ui::style::colors.view_text, dc.colors.alpha);
	const auto faint = core::color(ui::style::colors.view_text, 0.6f * dc.colors.alpha);
	const auto found = std::ranges::find(_model->years, row.year, &df::explore_year::year);
	if (found == _model->years.end()) return;
	const auto& year = *found;

	const auto year_text = core::to_string(row.year);
	const auto year_width =
		dc.measure_text(year_text, render::font_face::title, render::text_style::single_line, band.width()).cx;
	const recti year_bounds(band.left + _pad, band.top, band.left + _pad + year_width, band.bottom);
	dc.draw_text(year_text, year_bounds, render::font_face::title, render::text_style::single_line, text, {});

	const auto chip_pad = core::round(8 * _scale_factor);
	const recti count_bounds(year_bounds.right + chip_pad, band.top, band.right, band.bottom);
	dc.draw_text(core::format_count(year.counts.total()), count_bounds, render::font_face::dialog,
	             render::text_style::single_line, faint, {});

	if (const auto chips = _year_chips.find(row.year); chips != _year_chips.end())
	{
		for (const auto& chip : chips->second)
		{
			const auto& place = _model->year_places[chip.year_place];
			const auto bounds = chip.bounds.offset(band.left, band.top);
			const auto hovered = _hover.kind == explore_target_kind::year_place && _hover.index == chip.year_place;
			const auto color = place_color(place.place);

			dc.draw_rounded_rect(bounds,
			                     core::color(color.r, color.g, color.b, (hovered ? 0.65f : 0.32f) * dc.colors.alpha),
			                     bounds.height() / 2);
			dc.draw_text(place.place.name, bounds, render::font_face::dialog, render::text_style::single_line_center,
			             text, {});
		}
	}
}

void explore_view::render_days(render::draw_context& dc, const explore_timeline_layout::row& row,
                               const pointi offset) const
{
	const auto day_height = _layout.day_height;
	const auto bar_height = day_height >= 4 ? day_height - 1 : day_height;
	const auto view_top = _scroller.scroll_offset().y;
	const auto view_bottom = view_top + _timeline_content.height();
	const auto marking = !_marked_days.empty();
	const auto dim = marking ? 0.3f : 1.0f;
	const auto scale = std::log1p(static_cast<double>(_busiest_day));

	const auto photo = with_alpha(file_group::photo.color, dim * dc.colors.alpha);
	const auto video = with_alpha(file_group::video.color, dim * dc.colors.alpha);
	const auto other = with_alpha(file_group::other.color, dim * dc.colors.alpha);
	const auto photo_marked = with_alpha(file_group::photo.color, dc.colors.alpha);
	const auto video_marked = with_alpha(file_group::video.color, dc.colors.alpha);
	const auto other_marked = with_alpha(file_group::other.color, dc.colors.alpha);

	const auto first_visible = std::max(row.top, view_top);
	const auto last_visible = std::min(row.bottom, view_bottom);
	if (first_visible >= last_visible) return;

	const auto newest = row.last_day - static_cast<uint32_t>((first_visible - row.top) / day_height);
	const auto oldest_back = static_cast<uint32_t>((last_visible - row.top - 1) / day_height);
	const auto oldest = row.last_day - std::min(oldest_back, row.last_day - row.first_day);

	for (auto day = newest + 1; day-- > oldest;)
	{
		const auto& counts = _model->day_counts(day);
		const auto total = counts.total();
		if (total == 0) continue;

		const auto y = row.top + static_cast<int>(row.last_day - day) * day_height + offset.y;
		const auto width = std::max(2, core::round(_bars_width * std::log1p(static_cast<double>(total)) / scale));
		const auto photos = core::mul_div(width, static_cast<int>(counts.photos), static_cast<int>(total));
		const auto videos = core::mul_div(width, static_cast<int>(counts.videos), static_cast<int>(total));
		const auto x = offset.x + _bars_left;
		const auto marked = marking && std::ranges::binary_search(_marked_days, day);

		dc.draw_rect(recti(x, y, x + photos, y + bar_height), marked ? photo_marked : photo);
		dc.draw_rect(recti(x + photos, y, x + photos + videos, y + bar_height), marked ? video_marked : video);
		dc.draw_rect(recti(x + photos + videos, y, x + width, y + bar_height), marked ? other_marked : other);

		if (marked)
		{
			const auto tick = core::round(4 * _scale_factor);
			dc.draw_rect(recti(x - tick - core::round(3 * _scale_factor), y, x - core::round(3 * _scale_factor),
			                   y + bar_height),
			             core::color(explore_mark_color.r, explore_mark_color.g, explore_mark_color.b,
			                         dc.colors.alpha));
		}
	}
}

void explore_view::render_trips(render::draw_context& dc, const pointi offset, const recti clip) const
{
	const auto text = core::color(ui::style::colors.view_text, dc.colors.alpha);
	const auto faint = core::color(ui::style::colors.view_text, 0.62f * dc.colors.alpha);
	const auto pill_width = core::round(5 * _scale_factor);
	const auto min_height = core::round(5 * _scale_factor);
	const auto hovered_trip = _hover.kind == explore_target_kind::trip ? static_cast<int64_t>(_hover.index) : -1;

	for (const auto& piece : _layout.trips)
	{
		const auto& trip = _model->trips[piece.index];
		const auto color = place_color(trip.place);
		const auto hovered = static_cast<int64_t>(piece.index) == hovered_trip;
		const auto top = piece.top + offset.y;
		const auto bottom = std::max(piece.bottom, piece.top + min_height) + offset.y;
		if (bottom < clip.top || top > clip.bottom) continue;

		const recti pill(offset.x + _lane_left, top, offset.x + _lane_left + pill_width, bottom);
		dc.draw_rounded_rect(pill.inflate(hovered ? core::round(2 * _scale_factor) : 0, 0),
		                     core::color(color.r, color.g, color.b, (hovered ? 1.0f : 0.85f) * dc.colors.alpha),
		                     pill_width / 2);
	}

	for (const auto& label : _trip_labels)
	{
		const auto bounds = label.bounds.offset(offset);
		if (bounds.bottom < clip.top || bounds.top > clip.bottom) continue;

		const auto& trip = _model->trips[label.trip];
		const auto color = place_color(trip.place);
		const auto hovered = static_cast<int64_t>(label.trip) == hovered_trip;

		// Pushed down past the label before it, a label is tied back to its trip.
		const auto piece_y = label.piece_y + offset.y;
		const auto label_y = bounds.top + _line_height / 2 + core::round(2 * _scale_factor);

		if (std::abs(piece_y - label_y) > _line_height / 2)
		{
			dc.draw_line(pointd(offset.x + _lane_left + pill_width, piece_y), pointd(bounds.left - 2, label_y),
			             core::color(color.r, color.g, color.b, 0.6f * dc.colors.alpha), std::max(1.0, _scale_factor));
		}

		if (hovered)
		{
			dc.draw_rounded_rect(bounds.inflate(core::round(3 * _scale_factor), core::round(2 * _scale_factor)),
			                     core::color(color.r, color.g, color.b, 0.22f * dc.colors.alpha),
			                     core::round(4 * _scale_factor));
		}

		auto text_left = bounds.left;
		const auto picture = bounds.height();

		if (bounds.width() > picture * 3 && !trip.representative.path.is_empty())
		{
			render_thumbnail(dc, trip.representative.path,
			                 recti(bounds.left, bounds.top, bounds.left + picture, bounds.bottom),
			                 static_cast<float>(core::round(4 * _scale_factor)));
			text_left += picture + core::round(6 * _scale_factor);
		}

		// The name and how many items its search lists, then its days.
		const recti name_bounds(text_left, bounds.top, bounds.right, bounds.top + _line_height + 2);
		const auto name_width = dc.measure_text(trip.place.name, render::font_face::dialog,
		                                        render::text_style::single_line, name_bounds.width())
		                            .cx;
		dc.draw_text(trip.place.name, name_bounds, render::font_face::dialog, render::text_style::single_line, text,
		             {});
		dc.draw_text(std::format(" \xC2\xB7 {}", platform::format_number(core::to_string(trip.count))),
		             recti(name_bounds.left + name_width, name_bounds.top, name_bounds.right, name_bounds.bottom),
		             render::font_face::dialog, render::text_style::single_line, faint, {});

		const recti detail_bounds(text_left, name_bounds.bottom, bounds.right, bounds.bottom);
		dc.draw_text(format_days_in_year(trip.first_day, trip.last_day), detail_bounds, render::font_face::dialog,
		             render::text_style::single_line, faint, {});
	}
}

void explore_view::render_timeline(render::draw_context& dc)
{
	const auto bounds = _timeline_bounds;
	if (bounds.is_empty()) return;

	const render::scoped_clip clip(dc, bounds);
	dc.draw_rect(bounds, core::color(ui::style::colors.sidebar_background, dc.colors.alpha));
	const auto offset = timeline_offset();
	const auto content = _timeline_content;
	auto message_bounds = content;
	if (_timeline_info && _timeline_info->is_visible())
	{
		const auto info = _timeline_info->bounds.offset(offset);
		if (info.intersects(content))
		{
			_timeline_info->render(dc, offset);
			message_bounds.top = info.bottom + _pad;
		}
	}

	const auto text = core::color(ui::style::colors.view_text, dc.colors.alpha);
	const auto faint = core::color(ui::style::colors.view_text, 0.55f * dc.colors.alpha);

	if (!_model || _model->is_empty())
	{
		ui::draw_empty_message(dc, message_bounds.inflate(-_pad), _model ? tt.explore_empty : tt.explore_reading);
		return;
	}

	using kind = explore_timeline_layout::row_kind;
	const auto view_top = _scroller.scroll_offset().y;
	const auto view_bottom = view_top + content.height();
	const auto zebra = core::color(ui::style::colors.view_text, 0.035f * dc.colors.alpha);
	const auto highlight = core::color(ui::style::colors.important_background, 0.28f * dc.colors.alpha);
	const auto band_left = offset.x + _bars_left - core::round(4 * _scale_factor);
	const auto band_right = offset.x + _lane_left - core::round(6 * _scale_factor);

	{
		const render::scoped_clip content_clip(dc, content);

		// Alternate weeks are shaded, so a week reads as a block of days.
		for (const auto& week : _layout.weeks)
		{
			if (week.bottom < view_top || week.top > view_bottom) continue;
			if (week.index % 2 == 1)
				dc.draw_rect(recti(band_left, week.top + offset.y, band_right, week.bottom + offset.y), zebra);
		}

		// What the pointer, or the keyboard, or a drag across weeks, is on, wherever its days are drawn.
		const auto highlight_rows = [&](const int top, const int bottom, const bool with_gutter)
		{
			dc.draw_rect(recti(with_gutter ? offset.x + _pad / 2 : band_left, top + offset.y, band_right,
			                   bottom + offset.y),
			             highlight);
		};

		switch (_hover.kind)
		{
		case explore_target_kind::week:
		case explore_target_kind::weeks:
		{
			const auto older = _hover.kind == explore_target_kind::weeks ? _hover.other : _hover.index;
			if (const auto extent = weeks_extent(_hover.index, older))
				highlight_rows(extent->first, extent->second, false);
			break;
		}
		case explore_target_kind::month:
			if (const auto month = std::ranges::find(_layout.months, _hover.index,
			                                         &explore_timeline_layout::span::index);
			    month != _layout.months.end())
			{
				highlight_rows(month->top, month->bottom, true);
			}
			break;
		case explore_target_kind::trip:
			for (const auto& piece : _layout.trips)
			{
				if (piece.index == _hover.index) highlight_rows(piece.top, piece.bottom, false);
			}
			break;
		default: break;
		}

		if (_keyboard_used && _keyboard_week < _layout.weeks.size())
		{
			const auto& week = _layout.weeks[_keyboard_week];
			dc.draw_rect(recti(band_left, week.top + offset.y, band_right, week.bottom + offset.y),
			             core::color(ui::style::colors.important_background, 0.14f * dc.colors.alpha));
		}

		for (const auto& r : _layout.rows)
		{
			if (r.bottom < view_top || r.top > view_bottom) continue;

			switch (r.kind)
			{
			case kind::year: render_year_header(dc, r, r.top + offset.y); break;
			case kind::days: render_days(dc, r, offset); break;
			case kind::gap:
			case kind::empty_years:
			{
				const auto current = r.kind == kind::gap && r.last_day >= _model->today;
				const auto what = r.kind == kind::gap ? format_months(r.first_day, r.last_day)
				                                      : format_years(r.year, r.oldest_year);
				const auto message = str_format(current ? tt.explore_nothing_yet_in_fmt.sv()
				                                        : tt.explore_nothing_in_fmt.sv(),
				                                what);
				dc.draw_text(message,
				             recti(offset.x + _bars_left, r.top + offset.y, content.right - _pad, r.bottom + offset.y),
				             render::font_face::dialog, render::text_style::single_line, faint, {});
				break;
			}
			}
		}

		// Each month: a rule across where it starts, its name, its picture, and each week's count.
		const auto rule = core::color(ui::style::colors.view_text, 0.12f * dc.colors.alpha);

		for (const auto& span : _layout.months)
		{
			if (span.bottom < view_top - _thumbnail_height || span.top > view_bottom) continue;

			const auto& month = _model->months[span.index];
			const auto top = span.top + offset.y;
			const auto hovered = _hover.kind == explore_target_kind::month && _hover.index == span.index;

			dc.draw_rect(recti(offset.x + _pad, top, band_right, top + 1), rule);

			const recti label(offset.x + _pad, top + 2, offset.x + _pad + _gutter_width, top + 2 + _line_height);
			dc.draw_text(core::month(month.month, true), label, render::font_face::dialog,
			             render::text_style::single_line,
			             hovered ? text : core::color(ui::style::colors.view_text, 0.8f * dc.colors.alpha), {});

			const auto picture_height = std::min(_thumbnail_height, span.bottom - span.top - _line_height - 8);

			if (picture_height >= core::round(24 * _scale_factor) && !month.representative.path.is_empty())
			{
				const recti picture(label.left, label.bottom + 3, label.right, label.bottom + 3 + picture_height);
				render_thumbnail(dc, month.representative.path, picture,
				                 static_cast<float>(core::round(4 * _scale_factor)));
				if (hovered)
					dc.draw_rect(picture, core::color(ui::style::colors.important_background, 0.2f * dc.colors.alpha));
			}
		}

		for (const auto& week : _layout.weeks)
		{
			if (week.bottom < view_top || week.top > view_bottom || week.bottom - week.top < _line_height) continue;

			const auto total = _model->weeks[week.index].counts.total();
			if (total == 0) continue;

			const recti count(band_left, week.top + offset.y, band_right - core::round(4 * _scale_factor),
			                  week.bottom + offset.y);
			dc.draw_text(abbreviated_count(total), count, render::font_face::dialog,
			             render::text_style::single_line_far,
			             core::color(ui::style::colors.view_text, 0.38f * dc.colors.alpha), {});
		}

		render_trips(dc, offset, content);

		// The weeks a drag has taken in, and what releasing it would list.
		if (_choosing_weeks && _hover.kind == explore_target_kind::weeks)
		{
			const auto& newer = _model->weeks[_hover.index];
			const auto& older = _model->weeks[_hover.other];
			const auto counts = _model->span_counts(older.first_day, newer.last_day);
			const auto label = std::format("{} \xC2\xB7 {}", format_days(older.first_day, newer.last_day),
			                               format_plural_text(tt.title_item_count_fmt, counts.total()));

			if (const auto drawn = weeks_extent(_hover.index, _hover.other))
			{
				const auto top = drawn->first + offset.y;
				const auto extent = dc.measure_text(label, render::font_face::dialog, render::text_style::single_line,
				                                    content.width());
				const auto pad = core::round(6 * _scale_factor);
				auto box = recti(band_left, top - extent.cy - pad * 2, band_left + extent.cx + pad * 2, top);
				if (box.top < content.top) box = box.offset(0, content.top - box.top);
				dc.draw_rounded_rect(box, core::color(ui::style::colors.bubble_background, 0.95f * dc.colors.alpha),
				                     pad);
				dc.draw_text(label, box.inflate(-pad, 0), render::font_face::dialog, render::text_style::single_line,
				             text, {});
			}
		}

		// The year scrolled under the top keeps its header there.
		if (const auto* sticky = _layout.year_row_at(view_top); sticky && sticky->top < view_top)
		{
			render_year_header(dc, *sticky, content.top);
			const auto height = sticky->bottom - sticky->top;
			dc.draw_rect(recti(content.left, content.top + height, content.right, content.top + height + 1),
			             core::color(0.0f, 0.0f, 0.0f, 0.25f * dc.colors.alpha));
		}
	}

	// Where in time the place pointed at was visited, marked on the scroll bar as every scroll bar marks what its
	// content holds, however far it is from what is shown.
	std::vector<int> marked;
	marked.reserve(_marked_days.size());

	for (const auto day : _marked_days)
	{
		if (const auto top = _layout.top_of(day)) marked.push_back(*top);
	}

	std::ranges::sort(marked);
	_scroller.marks(std::move(marked), explore_mark_color.rgba());
	_scroller.draw_scroll(dc);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// What pointing at one half marks in the other
///////////////////////////////////////////////////////////////////////////////////////////////////

void explore_view::clear_marks()
{
	_cluster_marks.clear();
	_marked_days.clear();
	_marked_span = {};
}

void explore_view::mark_from_time(const uint32_t first_day, const uint32_t last_day, const df::explore_place* place)
{
	if (!_clusters || !_model || _clusters_of != _model) return;

	// The place's names as the model's located items index them.
	auto country = df::explore_no_place;
	auto state = df::explore_no_place;

	if (place)
	{
		for (uint32_t i = 0; i < _model->places.size(); ++i)
		{
			if (core::icmp(_model->places[i].sv(), place->country.sv()) == 0) country = i;
			if (place->level == df::location_level::state && core::icmp(_model->places[i].sv(), place->state.sv()) == 0)
				state = i;
		}
	}

	_cluster_marks.assign(_clusters->clusters.size(), 0);
	const auto [begin, end] = _model->located_between(first_day, last_day);

	for (auto i = begin; i < end; ++i)
	{
		const auto& located = _model->located[i];
		if (place &&
		    (located.country != country || (place->level == df::location_level::state && located.state != state)))
			continue;
		if (i < _clusters->of_located.size()) ++_cluster_marks[_clusters->of_located[i]];
	}
}

void explore_view::mark_from_cluster(const uint32_t cluster)
{
	if (!_clusters || !_model || _clusters_of != _model) return;

	for (size_t i = 0; i < _model->located.size() && i < _clusters->of_located.size(); ++i)
	{
		if (_clusters->of_located[i] != cluster) continue;

		const auto day = _model->located[i].day;
		if (day == 0) continue;
		if (_marked_days.empty() || _marked_days.back() != day) _marked_days.push_back(day);
	}

	if (!_marked_days.empty()) _marked_span = {_marked_days.front(), _marked_days.back()};
}

void explore_view::set_hover(const explore_target& target)
{
	if (target == _hover) return;

	_hover = target;
	_hover_turn_dwell.reset();
	_hover_turn_done = false;
	clear_marks();

	if (_model)
	{
		switch (target.kind)
		{
		case explore_target_kind::cluster: mark_from_cluster(target.index); break;
		case explore_target_kind::year:
			mark_from_time(_model->years[target.index].first_day, _model->years[target.index].last_day, nullptr);
			break;
		case explore_target_kind::year_place:
		{
			const auto& place = _model->year_places[target.index];
			mark_from_time(df::explore_day(place.year, 1, 1), df::explore_day(place.year, 12, 31), &place.place);
			break;
		}
		case explore_target_kind::month:
			mark_from_time(_model->months[target.index].first_day, _model->months[target.index].last_day, nullptr);
			break;
		case explore_target_kind::week:
			mark_from_time(_model->weeks[target.index].first_day, _model->weeks[target.index].last_day, nullptr);
			break;
		case explore_target_kind::weeks:
			mark_from_time(_model->weeks[target.other].first_day, _model->weeks[target.index].last_day, nullptr);
			break;
		case explore_target_kind::trip:
			mark_from_time(_model->trips[target.index].first_day, _model->trips[target.index].last_day,
			               &_model->trips[target.index].place);
			break;
		case explore_target_kind::none: break;
		}
	}

	_host->frame()->invalidate();
	_state.invalidate_view(view_invalid::tooltip);
}

void explore_view::open_target(const explore_target& target)
{
	if (!_model) return;

	switch (target.kind)
	{
	case explore_target_kind::cluster:
		if (_clusters && target.index < _clusters->clusters.size())
			open_search(df::map_area_search(cluster_area(target.index)));
		break;
	case explore_target_kind::year: open_search(df::explore_year_search(_model->years[target.index].year)); break;
	case explore_target_kind::year_place:
		open_search(df::explore_year_place_search(_model->year_places[target.index]));
		break;
	case explore_target_kind::month: open_search(df::explore_month_search(_model->months[target.index])); break;
	case explore_target_kind::week:
		open_search(
			df::explore_days_search(_model->weeks[target.index].first_day, _model->weeks[target.index].last_day));
		break;
	case explore_target_kind::weeks:
		open_search(
			df::explore_days_search(_model->weeks[target.other].first_day, _model->weeks[target.index].last_day));
		break;
	case explore_target_kind::trip: open_search(df::explore_trip_search(_model->trips[target.index])); break;
	case explore_target_kind::none: break;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Input
///////////////////////////////////////////////////////////////////////////////////////////////////

view_controller_ptr explore_view::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                           hit_test_context& ctx)
{
	if (_scroller.can_scroll() && ctx.occluded(_scroller.scroll_bounds()))
	{
		return std::make_shared<ui::scroll_controller>(host, _scroller, _scroller.scroll_bounds());
	}

	if (_globe_bounds.contains(loc))
	{
		ctx.confine(_globe_bounds);
		return std::make_shared<explore_controller>(host, shared_from_this(), pane::globe, _globe_bounds);
	}

	if (_timeline_content.contains(loc))
	{
		ctx.confine(_timeline_content);
		return std::make_shared<explore_controller>(host, shared_from_this(), pane::timeline, _timeline_content);
	}

	return nullptr;
}

void explore_view::hover(const pane p, const pointi loc)
{
	set_hover(p == pane::globe ? globe_target_at(loc) : timeline_target_at(loc));
}

void explore_view::press(const pane p, const pointi loc)
{
	_keyboard_used = false;
	_hover_turn_dwell.reset();

	if (p == pane::globe)
	{
		stop_turn();
		_pressed = globe_target_at(loc);
		_drag_start_view = _view;
		_drag_start_view_is_user_set = _view_is_user_set;
		_grabbed = projection().unproject(loc).value_or(gps_coordinate{});
		_turning = true;
		return;
	}

	_pressed = timeline_target_at(loc);
	_choosing_weeks = false;
	_weeks_scroll.reset();

	if (_pressed.kind == explore_target_kind::week) _weeks_anchor = _weeks_end = _pressed.index;
}

void explore_view::drag(const pane p, const pointi start, const pointi loc, const bool moved)
{
	if (!moved) return;

	if (p == pane::globe)
	{
		if (!_turning) return;

		set_hover({});
		const auto sphere = projection();
		std::optional<gps_coordinate> turned;

		if (_grabbed.is_valid())
		{
			// The point grabbed stays under the pointer, held to the disc once the pointer leaves it.
			auto unit_x = (loc.x - sphere.center().x) / sphere.radius();
			auto unit_y = (sphere.center().y - loc.y) / sphere.radius();
			const auto length = std::sqrt(unit_x * unit_x + unit_y * unit_y);

			if (length > 0.999)
			{
				unit_x *= 0.999 / length;
				unit_y *= 0.999 / length;
			}

			turned = globe_view_placing(_grabbed, unit_x, unit_y, _view);
		}
		else
		{
			// Grabbed off the sphere, the globe turns a radian for each radius of travel.
			const auto travel = loc - start;
			const auto degrees = gps_coordinate::rad2deg(1.0 / sphere.radius());
			turned = gps_coordinate(std::clamp(_drag_start_view.latitude() + travel.y * degrees, -90.0, 90.0),
			                        globe_wrap_longitude(_drag_start_view.longitude() - travel.x * degrees));
		}

		if (turned && !(*turned == _view))
		{
			_view = *turned;
			_view_is_user_set = true;
			_globe_invalid = true;
			_host->frame()->invalidate(_globe_bounds);
		}

		return;
	}

	if (_pressed.kind != explore_target_kind::week && !_choosing_weeks) return;
	_weeks_loc = loc;
	choose_weeks_at(loc);
}

// A drag down the bars takes in a run of weeks; held near an end, the timeline scrolls on at the pace a drag carries
// every list - from the tick while the pointer rests there - and the run follows.
void explore_view::choose_weeks_at(const pointi loc)
{
	if (const auto step = _weeks_scroll.step(loc.y, _timeline_content.top, _timeline_content.bottom, _scale_factor);
	    step != 0)
	{
		_scroller.offset(_host, 0, step);
	}

	const auto y = loc.y - timeline_offset().y;
	const explore_timeline_layout::span* nearest = nullptr;
	auto nearest_distance = std::numeric_limits<int>::max();

	for (const auto& week : _layout.weeks)
	{
		const auto distance = y < week.top ? week.top - y : y >= week.bottom ? y - week.bottom + 1 : 0;
		if (distance < nearest_distance)
		{
			nearest = &week;
			nearest_distance = distance;
		}
		if (distance == 0) break;
	}

	if (!nearest) return;

	_choosing_weeks = true;
	_weeks_end = nearest->index;
	set_hover({explore_target_kind::weeks, std::min(_weeks_anchor, _weeks_end), std::max(_weeks_anchor, _weeks_end)});
}

void explore_view::release(const pane p, const pointi loc, const bool moved)
{
	// A model that arrived while the button was down is taken up once it is let go.
	const core::scope_exit adopt([this]
	{
		if (_arrived) _state.invalidate_view(view_invalid::view_layout);
	});

	if (p == pane::globe)
	{
		const auto pressed = std::exchange(_pressed, {});
		_turning = false;
		_grabbed = {};
		if (moved) publish_globe_turn();

		// A drag is never read as a click.
		if (!moved && pressed.kind == explore_target_kind::cluster && globe_target_at(loc) == pressed)
		{
			open_target(pressed);
			return;
		}

		hover(p, loc);
		return;
	}

	const auto pressed = std::exchange(_pressed, {});

	if (std::exchange(_choosing_weeks, false))
	{
		const auto newer = std::min(_weeks_anchor, _weeks_end);
		const auto older = std::max(_weeks_anchor, _weeks_end);
		open_target(newer == older ? explore_target{explore_target_kind::week, newer}
		                           : explore_target{explore_target_kind::weeks, newer, older});
		return;
	}

	if (!moved && !pressed.is_none() && timeline_target_at(loc) == pressed)
	{
		open_target(pressed);
		return;
	}

	hover(p, loc);
}

bool explore_view::cancel(const pane p)
{
	const core::scope_exit adopt([this]
	{
		if (_arrived) _state.invalidate_view(view_invalid::view_layout);
	});
	_pressed = {};

	if (p == pane::globe)
	{
		if (!std::exchange(_turning, false)) return false;

		// Escape unwinds the turn and the claim on the view it made.
		_view = _drag_start_view;
		_view_is_user_set = _drag_start_view_is_user_set;
		_grabbed = {};
		_globe_invalid = true;
		_host->frame()->invalidate(_globe_bounds);
		return true;
	}

	if (!std::exchange(_choosing_weeks, false)) return false;
	set_hover({});
	return true;
}

void explore_view::leave(const pane /*p*/)
{
	if (!_turning && !_choosing_weeks) set_hover({});
}

bool explore_view::mouse_wheel(const pointi loc, const platform::wheel_notch notch)
{
	if (!notch.is_vertical()) return false;

	if (_globe_bounds.contains(loc))
	{
		// The wheel's delta reaches the view halved: a detent is 60, and each zooms one step.
		zoom_globe(std::pow(explore_zoom_per_notch, notch.delta / 60.0), loc);
		hover(pane::globe, loc);
		return true;
	}

	if (_timeline_bounds.contains(loc))
	{
		_scroller.offset(_host, 0, -notch.delta);
		hover(pane::timeline, loc);
		return _scroller.can_scroll();
	}

	return false;
}

// The keyboard walks the weeks: the arrows a week at a time, Page Up and Page Down a page, Home and End to
// either end, and Enter or Space opens the week it is on. Plus and minus zoom the globe.
bool explore_view::key_down(const char32_t key, const platform::key_state keys)
{
	if (keys.control || keys.alt) return false;

	if (key == platform::keys::OEM_PLUS || key == platform::keys::ADD)
	{
		step_globe_zoom(1);
		return true;
	}

	if (key == platform::keys::OEM_MINUS || key == platform::keys::SUBTRACT)
	{
		step_globe_zoom(-1);
		return true;
	}

	if (!_model || _layout.weeks.empty()) return false;

	const auto last = static_cast<int>(_layout.weeks.size()) - 1;
	const auto page = std::max(1, _timeline_content.height() / std::max(1, _layout.day_height * 7) - 2);

	if (key == platform::keys::RETURN || key == platform::keys::SPACE)
	{
		if (!_keyboard_used) return false;
		open_target(
			{explore_target_kind::week, _layout.weeks[std::min(_keyboard_week, static_cast<uint32_t>(last))].index});
		return true;
	}

	auto step = 0;
	if (key == platform::keys::UP) step = -1;
	else if (key == platform::keys::DOWN) step = 1;
	else if (key == platform::keys::PRIOR) step = -page;
	else if (key == platform::keys::NEXT) step = page;
	else if (key != platform::keys::HOME && key != platform::keys::END) return false;

	// The first press lands on the first week in sight, below the year header held at the top, so it never
	// carries the timeline away from what was being read.
	auto week = static_cast<int>(_keyboard_week) + step;

	if (!_keyboard_used)
	{
		const auto top = _scroller.scroll_offset().y + _layout.year_height;
		const auto in_sight = std::ranges::find_if(_layout.weeks, [top](const explore_timeline_layout::span& w)
		{ return w.top >= top; });
		week = in_sight == _layout.weeks.end() ? last : static_cast<int>(in_sight - _layout.weeks.begin());
	}

	if (key == platform::keys::HOME) week = 0;
	else if (key == platform::keys::END) week = last;

	_keyboard_week = static_cast<uint32_t>(std::clamp(week, 0, last));
	_keyboard_used = true;
	scroll_to_week(_layout.weeks[_keyboard_week].index);
	_host->frame()->invalidate();
	return true;
}

recti explore_view::keyboard_bounds() const
{
	if (!_keyboard_used || _keyboard_week >= _layout.weeks.size()) return {};
	return week_bounds(_layout.weeks[_keyboard_week].index);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Bubbles
///////////////////////////////////////////////////////////////////////////////////////////////////

void explore_view::add_bubble_thumbnail(view_hover_element& hover, const core::file_path& path)
{
	if (path.is_empty()) return;

	auto& t = _thumbnails[path];

	if (!t.item)
	{
		const auto indexed = _state.item_index.find_item(path);
		if (!indexed.ft) return;
		t.item = std::make_shared<df::item_element>(path, indexed);
	}

	const auto surface = t.item->thumbnail_surface();

	if (is_valid(surface))
	{
		hover.elements->add(std::make_shared<surface_element>(surface, core::round(160 * _scale_factor),
		                                                      flex_item::center | flex_item::new_line));
	}
	else if (is_valid(t.item->thumbnail()))
	{
		t.item->stage_thumbnail_surface(_state._async, false, true);
	}
	else if (!t.requested)
	{
		t.requested = true;
		_state.item_index.queue_load_thumbnail(t.item);
	}
}

recti explore_view::target_bounds(const explore_target& target) const
{
	if (!_model) return {};

	const auto offset = timeline_offset();
	const auto bars_left = _bars_left - core::round(4 * _scale_factor);
	const auto bars_right = _lane_left - core::round(6 * _scale_factor);

	switch (target.kind)
	{
	case explore_target_kind::week:
	case explore_target_kind::weeks:
	{
		const auto older = target.kind == explore_target_kind::weeks ? target.other : target.index;
		const auto drawn = weeks_extent(target.index, older);
		if (!drawn) return {};
		return recti(bars_left, drawn->first, bars_right, drawn->second).offset(offset);
	}
	case explore_target_kind::month:
	{
		const auto found = std::ranges::find(_layout.months, target.index, &explore_timeline_layout::span::index);
		if (found == _layout.months.end()) return {};
		return recti(_pad / 2, found->top, _bars_left - _pad / 2, found->bottom).offset(offset);
	}
	case explore_target_kind::trip:
	{
		const auto found = std::ranges::find(_trip_labels, target.index, &trip_label::trip);
		return found == _trip_labels.end() ? recti{} : found->bounds.offset(offset);
	}
	case explore_target_kind::year:
	case explore_target_kind::year_place:
	{
		const auto year = target.kind == explore_target_kind::year ? _model->years[target.index].year
		                                                           : _model->year_places[target.index].year;
		const auto* row = _layout.year_row(year);
		if (!row) return {};

		auto top = row->top + offset.y;
		const auto view_top = _scroller.scroll_offset().y;
		if (const auto* sticky = _layout.year_row_at(view_top); sticky == row && row->top < view_top)
			top = _timeline_content.top;

		const recti header(_timeline_content.left, top, _timeline_content.right, top + row->bottom - row->top);
		if (target.kind == explore_target_kind::year) return header;

		const auto chips = _year_chips.find(year);
		if (chips == _year_chips.end()) return header;
		const auto chip = std::ranges::find(chips->second, target.index, &year_chip::year_place);
		return chip == chips->second.end() ? header : chip->bounds.offset(header.left, header.top);
	}
	default: return {};
	}
}

void explore_view::bubble_time(view_hover_element& hover, const explore_target& target)
{
	if (!_model) return;

	// A bubble points at what it is about, so with nothing drawn to point at there is none.
	const auto bounds = target_bounds(target);
	if (bounds.is_empty()) return;

	std::string title;
	core::file_path picture;
	df::explore_counts counts;
	std::string action;
	uint32_t first_day = 0;
	uint32_t last_day = 0;
	uint32_t place_count = 0;
	const df::explore_place* place = nullptr;

	switch (target.kind)
	{
	case explore_target_kind::year:
	{
		const auto& year = _model->years[target.index];
		title = core::to_string(year.year);
		counts = year.counts;
		first_day = year.first_day;
		last_day = year.last_day;
		action = tt.explore_click_year;

		// The picture of the year's busiest month.
		uint32_t busiest = 0;
		for (const auto& month : _model->months)
		{
			if (month.year == year.year && month.counts.total() > busiest && !month.representative.path.is_empty())
			{
				busiest = month.counts.total();
				picture = month.representative.path;
			}
		}
		break;
	}
	case explore_target_kind::year_place:
	{
		const auto& year_place = _model->year_places[target.index];
		title = std::format("{}, {}", year_place.place.name, year_place.year);
		place = &year_place.place;
		place_count = year_place.count;
		action = tt.explore_click_place_year;

		for (const auto& trip : _model->trips)
		{
			if (trip.place.same_place(year_place.place) && df::explore_date(trip.first_day).year <= year_place.year &&
			    df::explore_date(trip.last_day).year >= year_place.year)
			{
				picture = trip.representative.path;
				break;
			}
		}
		break;
	}
	case explore_target_kind::month:
	{
		const auto& month = _model->months[target.index];
		title = format_month(month.year, month.month);
		picture = month.representative.path;
		counts = month.counts;
		first_day = month.first_day;
		last_day = month.last_day;
		action = tt.explore_click_month;
		break;
	}
	case explore_target_kind::week:
	{
		const auto& week = _model->weeks[target.index];
		title = format_days(week.first_day, week.last_day);
		picture = week.representative.path;
		counts = week.counts;
		first_day = week.first_day;
		last_day = week.last_day;
		action = tt.explore_click_week;
		break;
	}
	case explore_target_kind::weeks:
	{
		first_day = _model->weeks[target.other].first_day;
		last_day = _model->weeks[target.index].last_day;
		title = format_days(first_day, last_day);
		counts = _model->span_counts(first_day, last_day);
		picture = _model->weeks[target.index].representative.path;
		action = tt.explore_click_weeks;
		break;
	}
	case explore_target_kind::trip:
	{
		const auto& trip = _model->trips[target.index];
		title = trip.place.name;
		place = &trip.place;
		place_count = trip.count;
		picture = trip.representative.path;
		first_day = trip.first_day;
		last_day = trip.last_day;
		action = tt.explore_click_trip;
		break;
	}
	default: return;
	}

	hover.elements->add(make_icon_element(place ? icon_index::location : icon_index::time, flex_item::no_break));
	hover.elements->add(std::make_shared<text_element>(title, render::font_face::title, render::text_style::multiline,
	                                                   flex_item::line_break));

	if (place && first_day != 0)
	{
		hover.elements->add(std::make_shared<text_element>(format_days(first_day, last_day), render::font_face::dialog,
		                                                   render::text_style::multiline, flex_item::new_line));
	}

	add_bubble_thumbnail(hover, picture);

	if (place)
	{
		hover.elements->add(std::make_shared<text_element>(format_plural_text(tt.explore_items_there_fmt, place->name,
		                                                                      place_count, {}, 0),
		                                                   render::font_face::dialog, render::text_style::multiline,
		                                                   flex_item::new_line));
	}
	else
	{
		// Counted as the sidebar's pie counts them, by the names it gives each kind.
		const auto table = std::make_shared<ui::table_element>(flex_item::center | flex_item::new_line);
		const auto add = [&table](const file_group& group, const uint32_t n)
		{
			if (n == 0) return;
			table->add(std::make_shared<text_element>(platform::format_number(core::to_string(n)),
			                                          render::text_style::single_line_far),
			           std::make_shared<text_element>(group.display_name(n != 1)));
		};

		add(file_group::photo, counts.photos);
		add(file_group::video, counts.videos);
		add(file_group::other, counts.other);

		if (counts.total() == 0)
		{
			hover.elements->add(std::make_shared<text_element>(tt.explore_none_taken, render::font_face::dialog,
			                                                   render::text_style::multiline, flex_item::new_line));
		}
		else
		{
			hover.elements->add(table);
		}

		// The trips the time takes in.
		std::vector<std::string> places;
		for (const auto& trip : _model->trips)
		{
			if (trip.last_day < first_day || trip.first_day > last_day) continue;
			if (std::ranges::find(places, trip.place.name) == places.end()) places.push_back(trip.place.name);
			if (places.size() == 4) break;
		}

		if (!places.empty())
		{
			hover.elements->add(make_icon_element(icon_index::location, flex_item::new_line | flex_item::no_break));
			hover.elements->add(std::make_shared<text_element>(core::combine(places, ", ", false),
			                                                   render::font_face::dialog, render::text_style::multiline,
			                                                   flex_item::line_break));
		}
	}

	if (counts.total() > 0 || place_count > 0) hover.elements->add(std::make_shared<action_element>(action));

	hover.window_bounds = hover.active_bounds = bounds;
	hover.horizontal = true;
	hover.preferred_size = 240;
}

void explore_view::bubble(const pane p, view_hover_element& hover)
{
	if (_turning || _choosing_weeks || _hover.is_none()) return;

	hover.active_delay = bubble_delay::immediate;
	if (p == pane::globe && _hover.kind == explore_target_kind::cluster) bubble_cluster(hover, _hover.index);
	else if (p == pane::timeline) bubble_time(hover, _hover);
}

void explore_view::tick_hover_turn(const pointi loc, const uint64_t now)
{
	if (_hover_turn_done || _hover.is_none() || _hover.kind == explore_target_kind::cluster || _cluster_marks.empty())
		return;
	if (_turning || _choosing_weeks || !setting.show_help_tooltips || ui::command_active != 0 ||
	    timeline_target_at(loc) != _hover)
	{
		_hover_turn_dwell.reset();
		return;
	}

	const auto tolerance = std::max(1, core::round(4 * _scale_factor));
	if (_hover_turn_dwell.ready(now, loc, tolerance, bubble_delay::help))
	{
		_hover_turn_done = true;
		show_marked_places();
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Drawing
///////////////////////////////////////////////////////////////////////////////////////////////////

void explore_view::render(render::draw_context& dc, view_controller_ptr /*controller*/)
{
	render_globe(dc);
	render_timeline(dc);

	// The rule between the halves.
	dc.draw_rect(recti(_timeline_bounds.left, _timeline_bounds.top, _timeline_bounds.left + 1, _timeline_bounds.bottom),
	             core::color(0.0f, 0.0f, 0.0f, 0.35f * dc.colors.alpha));
}
