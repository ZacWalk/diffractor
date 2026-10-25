// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Map control dialog widget for location picking in metadata edit and
// advanced search dialogs. Adds clustered item markers, hover previews and a
// picked-cluster highlight on top of the shared map engine.

#pragma once

#include "ui_map_common.h"

class map_control final : public view_element, public std::enable_shared_from_this<map_control>, public ui::frame_host
{
public:
	ui::control_frame_ptr _owner;
	ui::frame_ptr _frame;
	ui::bubble_window_ptr _bubble;
	sizei _extent;
	async_strategy& _async;
	std::function<void(gps_coordinate)> _cb;

	std::unique_ptr<map_engine> _engine;

	bool _hover = false;
	bool _panned = false;
	pointi _start_loc;

	int _hover_marker = -1;
	int _hover_count = 0;
	pointi _hover_anchor;
	uint32_t _view_generation = 1;

	// Populates the hover bubble for a clustered marker. Supplied by the owning dialog
	// so this control does not need to know about items or the index. Setting
	// `needs_refresh` asks for the bubble to be rebuilt on the next tick, which is how a
	// thumbnail that is still loading eventually appears.
	std::function<void(view_hover_element&, int marker_index, int count, pointi anchor, bool& needs_refresh)>
	marker_hover;

	// Raised after the user zooms so the owner can rebuild markers for the new zoom.
	std::function<void(int zoom)> zoom_changed;

	// Raised when the user clicks a cluster, with the ground radius that bubble covers so
	// the owner can search exactly the area the user pointed at.
	std::function<void(gps_coordinate coord, double radius_km, int count)> marker_picked;

	// Set from inside marker_hover when the bubble content is still incomplete (a
	// thumbnail is loading); the bubble is then rebuilt on the next tick.
	bool hover_needs_refresh = false;

	map_control(async_strategy& async, std::function<void(gps_coordinate)> cb) : _async(async), _cb(
			std::move(cb))
	{
	}

	void init(const ui::control_frame_ptr& owner)
	{
		_owner = owner;

		// A slow timer drives tick(), which is the only chance this control has to notice
		// that an asynchronously loaded hover thumbnail has arrived.
		ui::frame_style style;
		style.timer_milliseconds = 250;

		_frame = owner->create_frame(weak_from_this(), style);
		_engine = std::make_unique<map_engine>(_async, [this] { frame()->invalidate(); });
	}

	// Never null: the control is measured, laid out and asked to repaint before init() runs.
	const ui::frame_ptr& frame() const
	{
		return _frame ? _frame : ui::no_frame();
	}

	sizei measure(ui::measure_context& mc, const int cx) const override
	{
		return {cx, cx};
	}

	void visit_controls(const std::function<void(const ui::control_base_ptr&)>& handler) override
	{
		handler(frame());
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;
		positions.emplace_back(frame(), bounds, is_visible());
	}

	void on_window_layout(ui::measure_context& mc, const sizei extent, const bool is_minimized) override
	{
		const bool extent_changed = _extent != extent;
		_extent = extent;

		// The initial location is typically set (via set_location_marker) before the
		// dialog has been laid out, while _extent is still empty and tile fetching is a
		// no-op. The first real layout is therefore the earliest point at which we can
		// fetch the surrounding tiles, mirroring map_view::activate for the full view.
		if (_engine && extent_changed && !extent.is_empty())
		{
			if (!apply_pending_frame())
			{
				_engine->fetch_tiles_for_bounds(calc_bounds());
			}
		}
	}

	void on_mouse_move(const pointi loc, const bool is_tracking) override
	{
		if (!_hover)
		{
			_hover = true;
			frame()->invalidate();
		}

		if (is_tracking && _engine)
		{
			const auto drag = loc - _start_loc;
			if (std::abs(drag.x) > 2 || std::abs(drag.y) > 2) _panned = true;

			hide_marker_bubble();
			_engine->pan(_start_loc, loc, calc_bounds());
		}
		else
		{
			update_marker_hover(loc);
		}
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_start_loc = loc;
		_panned = false;
		if (_engine)
			_engine->pan_start();
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		if (_engine)
		{
			pointi anchor;
			int count = 0;
			const auto on_marker = !_panned && _engine->hit_test_marker(loc, _extent, anchor, count) >= 0;
			const auto marker_gps = on_marker ? _engine->gps_at_screen(anchor, _extent) : gps_coordinate{};

			_engine->pan_end(_start_loc, loc, calc_bounds());

			if (on_marker)
			{
				// The map stays where it is: the user picked a bubble, not a new view.
				hide_marker_bubble();
				_engine->set_selected(marker_gps, count);
				if (marker_picked) marker_picked(marker_gps, _engine->cluster_radius_km(marker_gps), count);
			}
			else if (_panned)
			{
				// A click that neither hit a cluster nor moved the map has chosen nothing.
				++_view_generation;
				send_location_changed_event(_engine->location());
			}
		}

		_panned = false;
		_start_loc = {0, 0};
	}

	void on_mouse_leave(const pointi loc) override
	{
		hide_marker_bubble();

		if (_hover)
		{
			_hover = false;
			frame()->invalidate();
		}
	}

	void send_location_changed_event(const gps_coordinate& loc) const
	{
		if (_cb)
		{
			_cb(loc);
		}
	}

	void on_mouse_wheel(const pointi loc, const int delta, const ui::key_state keys, bool& was_handled) override
	{
		if (_engine)
		{
			hide_marker_bubble();
			was_handled = _engine->zoom(delta, calc_bounds());

			if (was_handled && zoom_changed)
			{
				++_view_generation;
				zoom_changed(_engine->zoom_level());
			}
		}
	}

	void tick() override
	{
		if (_hover_marker >= 0 && hover_needs_refresh)
		{
			show_marker_bubble();
		}
	}

	void activate(bool is_active) override
	{
	}

	bool key_down(const int c, const ui::key_state keys) override
	{
		return false;
	}

	void on_window_paint(ui::draw_context& dc) override
	{
		if (_engine)
		{
			_engine->render(dc, _extent);
		}
	}

	recti calc_bounds() const
	{
		return recti(_extent);
	}

	void set_location_marker(const gps_coordinate loc)
	{
		if (_engine)
		{
			_engine->set_location(loc, calc_bounds());
		}
	}

	// Frames the map on the area that holds something worth clicking. Owners know the box
	// before the dialog has been laid out, when the extent is still empty and framing would
	// have nothing to fit into, so the request is held until the first real layout.
	void frame_on(const map_box& box)
	{
		_pending_frame = box;
		apply_pending_frame();
	}

	// Clustered item locations to draw on the map. Built by the owner off the UI thread
	// and published here as a detached snapshot.
	void set_markers(const std::vector<map_engine::marker>& markers)
	{
		if (_engine)
		{
			hide_marker_bubble();
			_engine->set_markers(markers);
		}
	}

	void set_marker_snapshot(map_engine::marker_snapshot markers)
	{
		if (_engine)
		{
			hide_marker_bubble();
			_engine->set_marker_snapshot(std::move(markers));
		}
	}

	uint32_t view_generation() const
	{
		return _view_generation;
	}

	std::vector<gps_coordinate> visible_cluster_coordinates() const
	{
		return _engine ? _engine->visible_cluster_coordinates(_extent) : std::vector<gps_coordinate>{};
	}

	gps_coordinate gps_at_screen(const pointi loc) const
	{
		return _engine ? _engine->gps_at_screen(loc, _extent) : gps_coordinate{};
	}

	void set_selected(const gps_coordinate loc, const int count)
	{
		if (_engine)
		{
			_engine->set_selected(loc, count);
		}
	}

	void set_show_crosshair(const bool show)
	{
		if (_engine)
		{
			_engine->set_show_crosshair(show);
		}
	}

	int zoom_level() const
	{
		return _engine ? _engine->zoom_level() : 0;
	}

private:
	map_box _pending_frame;

	// Returns false when there was nothing to frame, so the caller can fall back to a plain
	// tile fetch. Framing replaces the view, so owners always get a chance to rebuild their
	// markers for it, even when the fitted zoom happens to match the one already showing.
	bool apply_pending_frame()
	{
		if (!_engine || !_pending_frame.valid || _extent.is_empty()) return false;

		const auto framed = _engine->fit_box(_pending_frame, calc_bounds());
		_pending_frame = {};

		if (framed)
		{
			hide_marker_bubble();
			++_view_generation;

			if (zoom_changed)
			{
				zoom_changed(_engine->zoom_level());
			}
		}

		return framed;
	}

	void update_marker_hover(const pointi loc)
	{
		if (!_engine) return;

		pointi anchor;
		int count = 0;
		const auto marker = _engine->hit_test_marker(loc, _extent, anchor, count);

		if (marker == _hover_marker) return;

		_hover_marker = marker;
		_hover_anchor = anchor;
		_hover_count = count;

		show_marker_bubble();
	}

	void show_marker_bubble()
	{
		if (!_owner || !_frame) return;

		if (_hover_marker < 0 || !marker_hover)
		{
			hide_marker_bubble();
			return;
		}

		if (!_bubble) _bubble = _owner->create_bubble();
		if (!_bubble) return;

		view_hover_element hover;
		hover.clear();
		hover_needs_refresh = false;
		marker_hover(hover, _hover_marker, _hover_count, _hover_anchor, hover_needs_refresh);

		if (hover.is_empty())
		{
			_bubble->hide();
			return;
		}

		// The bubble is a top-level window, so translate the anchor from map-local
		// coordinates into screen coordinates before showing it.
		const auto offset = frame()->window_bounds().top_left();
		_bubble->show(hover.elements, hover.window_bounds.offset(offset), hover.x_focus, hover.preferred_size,
		              hover.horizontal);
	}

	void hide_marker_bubble()
	{
		_hover_marker = -1;
		_hover_count = 0;
		hover_needs_refresh = false;
		if (_bubble) _bubble->hide();
	}
};

using map_control_ptr = std::shared_ptr<map_control>;
