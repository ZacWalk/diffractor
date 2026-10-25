// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Map view base class. Delegates tile rendering, pan, zoom, and
// crosshair logic to a map_engine. Subclasses add domain logic.

#pragma once

#include "model.h"
#include "ui_view.h"
#include "ui_map_common.h"

class map_view;

class map_pan_controller final : public view_controller
{
	map_view& _view;
	bool _tracking = false;
	bool _panned = false;
	int _hover_marker = -1;

public:
	map_pan_controller(map_view& view, const view_host_ptr& host, recti bounds);

	ui::style::cursor cursor() const override
	{
		return _tracking ? ui::style::cursor::hand_up : ui::style::cursor::hand_down;
	}

	void on_mouse_left_button_down(pointi loc, ui::key_state keys) override;
	void on_mouse_move(pointi loc) override;
	void on_mouse_left_button_up(pointi loc, ui::key_state keys) override;
	void popup_from_location(view_hover_element& hover) override;
};

class map_view : public view_base
{
protected:
	view_state& _state;
	view_host_ptr _host;
	sizei _extent;
	map_engine _engine;

	friend class map_pan_controller;

public:
	map_view(view_state& state, view_host_ptr host)
		: _state(state), _host(std::move(host)),
		  _engine(state._async, [this] { _host->frame()->invalidate(); })
	{
	}

	void set_map_location(const gps_coordinate loc)
	{
		_engine.set_location(loc, recti(_extent));
	}

	// --- view_base overrides ---

	void activate(const sizei extent) override
	{
		_extent = extent;
		_state.stop();
		_engine.fetch_tiles_for_bounds(recti(_extent));
	}

	void deactivate() override
	{
		_engine.clear_caches();
	}

	void layout(ui::measure_context& mc, const sizei extent) override
	{
		_extent = extent;
	}

	void render(ui::draw_context& dc, view_controller_ptr controller) override
	{
		_engine.render(dc, _extent);
	}

	void broadcast_event(const view_element_event& event) const override
	{
		if (event.type == view_element_event_type::free_graphics_resources)
		{
			_engine.free_graphics_resources();
		}
	}

	// The map's primary axis and its scale are the same thing, so the modifier changes nothing here.
	bool mouse_wheel(const pointi loc, const ui::wheel_notch notch) override
	{
		if (!notch.is_vertical()) return false;
		if (_engine.zoom(notch.delta, recti(_extent))) on_map_zoomed(_engine.zoom_level());
		return true;
	}

	void pan_start(const pointi start_loc) override
	{
		_engine.pan_start();
	}

	void pan(const pointi start_loc, const pointi current_loc) override
	{
		_engine.pan(start_loc, current_loc, recti(_extent));
	}

	void pan_end(const pointi start_loc, const pointi final_loc) override
	{
		const auto gps = _engine.pan_end(start_loc, final_loc, recti(_extent));
		on_map_panned(gps);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             hit_test_context& ctx) override
	{
		return std::make_shared<map_pan_controller>(*this, host, recti(_extent));
	}

	// Called after pan completes with the new center GPS. Override to handle reverse geocoding etc.
	virtual void on_map_panned(const gps_coordinate& new_center)
	{
	}

	virtual void on_map_zoomed(int zoom)
	{
	}

	// Called when the cursor hovers an aggregated marker cluster. `marker_index` is
	// the representative marker index (as passed to map_engine::set_markers), `count`
	// the number of aggregated items, and `anchor` the cluster's screen position.
	// Override to populate the hover bubble (e.g. with a thumbnail). Default: no popup.
	virtual void on_marker_hover(view_hover_element& hover, int marker_index, int count, pointi anchor)
	{
	}

	// Called when a click (not a drag) lands on a marker cluster, with the cluster's coordinate
	// and aggregated count. Override where the centre is the answer. Default: no action.
	virtual void on_marker_clicked(const gps_coordinate& coordinate, int count)
	{
	}
};

// --- map_pan_controller inline implementations (after map_view is complete) ---

inline map_pan_controller::map_pan_controller(map_view& view, const view_host_ptr& host, const recti bounds)
	: view_controller(host, bounds), _view(view)
{
}

inline void map_pan_controller::on_mouse_left_button_down(const pointi loc, const ui::key_state keys)
{
	view_controller::on_mouse_left_button_down(loc, keys);
	_tracking = true;
	_panned = false;
	_view.pan_start(loc);
}

inline void map_pan_controller::on_mouse_move(const pointi loc)
{
	_last_loc = loc;

	if (_tracking)
	{
		const auto drag = loc - _start_loc;
		if (std::abs(drag.x) > 2 || std::abs(drag.y) > 2) _panned = true;
		_view.pan(_start_loc, loc);
		return;
	}

	// Track which marker cluster is under the cursor. When it changes we must
	// re-evaluate the hover bubble: the map controller persists across mouse moves
	// (its bounds cover the whole view), so unlike a transient controller it never
	// re-fires controller_changed(). Invalidating the tooltip re-runs
	// app_frame::update_tooltip -> popup_from_location so the bubble shows/hides.
	pointi anchor;
	int count = 0;
	const int marker = _view._engine.hit_test_marker(loc, _view._extent, anchor, count);

	if (marker != _hover_marker)
	{
		_hover_marker = marker;

		if (_host)
		{
			_host->invalidate_view(view_invalid::tooltip | view_invalid::view_redraw);
		}
	}
}

inline void map_pan_controller::popup_from_location(view_hover_element& hover)
{
	pointi anchor;
	int count = 0;
	const int marker = _view._engine.hit_test_marker(_last_loc, _view._extent, anchor, count);

	if (marker >= 0)
	{
		_view.on_marker_hover(hover, marker, count, anchor);
	}
}

inline void map_pan_controller::on_mouse_left_button_up(const pointi loc, const ui::key_state keys)
{
	if (_tracking)
	{
		_tracking = false;

		// Resolved before pan_end, which moves the map and invalidates the cluster anchors.
		pointi anchor;
		int count = 0;
		const auto on_marker = !_panned && _view._engine.hit_test_marker(loc, _view._extent, anchor, count) >= 0;
		const auto marker_gps = on_marker ? _view._engine.gps_at_screen(anchor, _view._extent) : gps_coordinate{};

		_view.pan_end(_start_loc, loc);

		if (on_marker && marker_gps.is_valid())
		{
			_hover_marker = -1;
			_view.on_marker_clicked(marker_gps, count);
		}

		_panned = false;
	}
}
