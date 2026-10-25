// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Compact horizontal item selector used by edit and locate workflows.

#pragma once

#include "ui_view.h"

class selector_view final : public view_base, public std::enable_shared_from_this<selector_view>
{
public:
	using select_item_fn = std::function<void(const df::item_element_ptr&, ui::key_state)>;
	using item_filter_fn = std::function<bool(const df::item_element_ptr&)>;

private:
	struct selector_item
	{
		df::item_element_ptr item;
		recti bounds;
		ui::const_image_ptr image;
		ui::texture_ptr texture;
		// Decoded on a worker and published here; the texture is created on the next paint because a
		// device resource may only be made on the UI thread.
		ui::surface_ptr surface;
		bool decode_pending = false;
		// Terminal: a decode that returned nothing will return nothing again for the same image, and
		// paint is what asks. Without this the strip re-requests on every frame it repaints.
		bool decode_failed = false;
		uint64_t decode_request = 0;

		bool has_retained_resource() const
		{
			return texture || surface || decode_pending;
		}

		void retire_resources()
		{
			image.reset();
			texture.reset();
			surface.reset();
			decode_pending = false;
			decode_failed = false;
			++decode_request;
		}
	};

	struct rebuild_stats
	{
		size_t lookup_entries = 0;
		size_t probes = 0;
		size_t reused = 0;
		size_t preserved_markers = 0;
	};

	struct resource_stats
	{
		size_t before = 0;
		size_t after_retire = 0;
		size_t after_stale_publish = 0;
		size_t after_reentry_publish = 0;
	};

	view_state& _state;
	view_host_ptr _host;
	select_item_fn _select_item;
	item_filter_fn _item_filter;
	std::vector<selector_item> _items;
	df::item_element_ptr _selection_anchor;
	sizei _extent;
	int _scroll_x = 0;
	int _content_width = 0;
	int _gap = 0;
	int _scrollbar_height = 0;
	bool _active = false;
	// Item bounds only exist once layout has run, so activation defers the scroll that reveals focus.
	bool _scroll_to_focus = false;

	void rebuild_items();
	static rebuild_stats rebuild_selector_items(std::vector<selector_item>& existing,
	                                            const std::vector<df::item_element_ptr>& ordered,
	                                            std::vector<selector_item>& items);
	static size_t retire_off_band_resources(std::vector<selector_item>& items, recti logical_bounds);
	static bool publish_decoded_surface(std::vector<selector_item>& items, recti logical_bounds,
	                                    const df::item_element_ptr& item, const ui::const_image_ptr& image,
	                                    uint64_t request, ui::surface_ptr surface);
	void clamp_scroll();
	void update_visible_items();
	recti resource_logical_bounds() const;
	void retire_off_band_resources();
	bool publish_decoded_surface(const df::item_element_ptr& item, const ui::const_image_ptr& image,
	                             uint64_t request, ui::surface_ptr surface);
	selector_item* item_from_location(pointi loc);
	const selector_item* item_from_location(pointi loc) const;

public:
	selector_view(view_state& state, view_host_ptr host, select_item_fn select_item);
	void filter(item_filter_fn item_filter);
	void reset_selection_anchor() { _selection_anchor.reset(); }
	void selection_anchor(df::item_element_ptr item) { _selection_anchor = std::move(item); }
	df::item_elements selection_range(const df::item_element_ptr& item) const;
	static quadd thumbnail_destination(sizei texture_dimensions, recti image_bounds, ui::orientation orientation,
	                                   bool show_rotated);

	void activate(sizei extent) override;
	void deactivate() override;
	void refresh() override;
	void items_changed(bool path_changed) override;
	void display_changed() override;
	void layout(ui::measure_context& mc, sizei extent) override;
	void render(ui::draw_context& dc, view_controller_ptr controller) override;
	bool mouse_wheel(pointi loc, ui::wheel_notch notch) override;
	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, hit_test_context& ctx) override;
	void broadcast_event(const view_element_event& event) const override;

	void make_visible(const df::item_element_ptr& item);
	bool can_scroll() const;
	recti scrollbar_bounds() const;
	recti scrollbar_thumb_bounds() const;
	void scrollbar_to(int x);
	void scroll_by(int delta_x);

	static rebuild_stats test_rebuild_reuse(size_t count);
	static resource_stats test_resource_retirement(size_t count, sizei extent, int item_width, int scroll_x);
	void test_add_pending_decode_for_resource_event();
	bool test_resource_event_cleared_pending_decode() const;
};
