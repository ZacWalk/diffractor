// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Location assignment view. Provides a map-based interface for setting
// GPS coordinates on selected items, with search and reverse geocoding.
// The map fills the main view area (left panel) and search/properties are in
// the right panel (view_controls).

#pragma once

#include "view_map.h"

class view_controls_host;
using view_controls_host_ptr = std::shared_ptr<view_controls_host>;
class locate_auto_complete_strategy;

namespace ui
{
	class complete_strategy_t;
}

class locate_view final :
	public map_view,
	public std::enable_shared_from_this<locate_view>
{
	std::string _title;
	std::string _status;
	std::shared_ptr<selected_location_t> _location;
	std::function<void()> _populate_controls;
	// Monotonic request id used to discard stale reverse-geocode results when the
	// user pans the map quickly and multiple background requests are in flight.
	uint64_t _geocode_request_id = 0;
	// The place search behind the panel's search box, owned by that box. An explicit choice on the
	// map supersedes whatever it still has in flight.
	std::weak_ptr<locate_auto_complete_strategy> _place_search;
	// Composed by the location worker: the qualified place name already carrying its Near or
	// Remote form, and the locations.md 2.7 bearing descriptor for anything not `at` a place.
	std::string _place_label;
	std::string _bearing;

	struct marker_item
	{
		df::file_path path;
		df::item_element_ptr item;
	};

	// Geotagged collection/current-list items shown as aggregated markers. Collection-only
	// entries stay lightweight until hovered; current-list entries reuse their item element.
	std::vector<marker_item> _marker_items;
	std::vector<marker_item> _pending_marker_items;
	uint32_t _pending_marker_generation = 0;
	// Collection-wide markers are built off the UI thread, so a result that lands after the
	// user has zoomed again describes a map that no longer exists.
	uint32_t _marker_generation = 0;
	df::unique_paths _thumbnail_requests;
	bool run(detach_file_handles& detach);

public:
	locate_view(view_state& state, view_host_ptr host) : map_view(state, std::move(host))
	{
		_location = std::make_shared<selected_location_t>();
	}

	// False when nothing was written, so the caller can keep the user on the item that failed.
	bool run();
	void run_and_next(bool forward);
	void refresh() override;

	bool can_run() const
	{
		return gps_coordinate(_location->latitude, _location->longitude).is_valid() &&
			!_state.selected_items().empty();
	}

	std::string_view status() override
	{
		return _status;
	}

	void activate(sizei extent) override;
	void deactivate() override;
	void display_changed() override;

	void on_map_panned(const gps_coordinate& new_center) override;
	void on_map_zoomed(int zoom) override;
	void on_marker_hover(view_hover_element& hover, int marker_index, int count, pointi anchor) override;
	void on_marker_clicked(const gps_coordinate& coordinate, int count) override;

	view_controls_host_ptr controls(const ui::control_frame_ptr& owner);

	void exit() override
	{
		_state.view_mode(view_type::items);
	}

	std::string_view title() override
	{
		_title = std::format("{}: {}", s_app_name, tt.location_title);
		return _title;
	}

	const std::shared_ptr<selected_location_t>& location() const { return _location; }
	void update_location(const location_t& loc);

	// The place search behind the panel, while the panel exists.
	std::shared_ptr<ui::complete_strategy_t> place_search_for_tests() const;

private:
	void rebuild_markers();
	void select_default_location();
	// Forget the resolved place so a stale name is never shown against a new coordinate, and
	// never written by a Run that happens between the move and the lookup that follows it.
	void clear_resolved_place();
	// An answer the place search still owes would select its first match over this choice.
	void supersede_place_search() const;
	// Queue an async reverse-geocode lookup for the given GPS position. Tags the
	// request with a monotonic id so stale results from earlier requests are
	// discarded when the user pans rapidly or makes an explicit selection.
	void request_reverse_geocode(const gps_coordinate& gps);
};
