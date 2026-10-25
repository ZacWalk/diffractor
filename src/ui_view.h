// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: View framework and element hosting. Defines view_element base class,
// view_controller for interactions, hit_test_context, and view hosting infrastructure.

#pragma once

class view_controller;
class view_element;
class view_elements;
class view_host;
class view_host_base;
class view_state;
enum class render_valid;
struct hit_test_context;
struct view_hover_element;

using view_host_ptr = std::shared_ptr<view_host>;
using view_controller_ptr = std::shared_ptr<view_controller>;
using view_element_ptr = std::shared_ptr<view_element>;
using view_host_base_ptr = std::shared_ptr<view_host_base>;

enum class menu_type
{
	view,
	sidebar,
	media,
	items,
	movie
};

enum class view_element_event_type
{
	invoke,
	click,
	double_click,
	tick,
	populate,
	dpi_changed,
	initialise,
	update_command_state,
	free_graphics_resources,
};

enum class view_invalid
{
	none = 0,

	view_layout = 1 << 1,
	// The selector strip's membership rule changed, so rebuild the strip without re-grouping.
	selector_filter = 1 << 2,
	group_layout = 1 << 3,
	app_layout = 1 << 4,
	view_redraw = 1 << 5,
	item_scan = 1 << 6,
	media_elements = 1 << 7,
	tooltip = 1 << 8,
	command_state = 1 << 9,
	sidebar = 1 << 10,
	sidebar_file_types_and_dates = 1 << 11,
	presence = 1 << 12,
	address = 1 << 13,
	controller = 1 << 14,
	index = 1 << 15,
	refresh_items = 1 << 16,
	focus_item_visible = 1 << 17,
	screen_saver = 1 << 18,
	options_save = 1 << 19,
	index_summary = 1 << 20,
	group_layout_complete = 1 << 21,
	animations = 1 << 22,
	selection_list = 1 << 23,
	font_size = 1 << 24,
	image_compare = 1 << 25,
	status = 1 << 26,
	// Like index, but first forgets every cached scan result so the collection is re-read from the files.
	index_rebuild = 1 << 27,
	// Re-run the per-item sidebar sums without rebuilding the sidebar's elements. Each sum is an index
	// query, so what changed the counts must not also discard every element and its text layout.
	sidebar_counts = 1 << 28,
	// Re-enumerate volumes and rebuild with the result. Only a drive event earns this: the scan blocks
	// on unreachable network mappings, so it must not ride along with every index invalidation.
	sidebar_drives = 1 << 29,

	// No sidebar flag: these change how the sidebar is drawn, not what it says. font_size already
	// re-measures it, and a recount here would query the index once per row for an unchanged answer.
	visual_options = view_layout |
	group_layout |
	app_layout |
	options_save |
	command_state |
	tooltip |
	address |
	font_size |
	media_elements,

	options = view_layout |
	group_layout |
	app_layout |
	options_save |
	sidebar |
	command_state |
	tooltip |
	refresh_items |
	address |
	font_size,
};

constexpr view_invalid operator|(const view_invalid a, const view_invalid b)
{
	return static_cast<view_invalid>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

constexpr view_invalid operator&(const view_invalid a, const view_invalid b)
{
	return static_cast<view_invalid>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

constexpr bool operator&&(const view_invalid a, const view_invalid b)
{
	return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

constexpr view_invalid& operator|=(view_invalid& a, const view_invalid b)
{
	a = static_cast<view_invalid>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	return a;
}

struct view_element_event
{
	view_element_event_type type;
	view_host_ptr host;
};

struct interaction_context
{
	const pointi loc;
	const pointi element_offset;
	const bool tracking;
	bool invalidate_view = false;
};

// Threaded through one hit test. It accumulates the region within which view_host may reuse the
// controller the test produces: anything tested at higher precedence and passed over is cut out of
// it, so moving the pointer onto that region forces a fresh test. Recording is a side effect of
// testing, which is what stops a new tool from being hit tested but forgotten here.
//
// Every rect is device space, the space _loc is in.
struct hit_test_context
{
	hit_test_context(const pointi loc, const recti extent) : _loc(loc), _stability(extent)
	{
	}

	// The region a controller found at this location remains the right answer for.
	recti stability() const
	{
		return _stability;
	}

	// Answers whether the pointer is inside a region that takes precedence, so nothing beneath it
	// may claim the pointer. When it is not, the region is cut away instead.
	bool occluded(const recti bounds)
	{
		if (bounds.is_empty()) return false;
		if (bounds.contains(_loc)) return true;
		_stability.exclude(_loc, bounds);
		return false;
	}

	// For a region that was tested and missed by some other means, so it still has to be cut away.
	// Prefer occluded(): a caller that tests one way and records another can disagree with itself.
	void passed_over(const recti bounds)
	{
		if (!bounds.is_empty()) _stability.exclude(_loc, bounds);
	}

	// Narrows the region to one the answer provably cannot travel outside of, for a caller that has
	// already chosen between panes or regions rather than between overlapping targets.
	void confine(const recti bounds)
	{
		if (!bounds.is_empty()) _stability = _stability.intersection(bounds);
	}

private:
	pointi _loc;
	recti _stability;
};

inline ui::color view_handle_color(const bool selected, const bool hover, const bool tracking,
                                      const bool view_has_focus, const bool text_over,
                                      const ui::color bg_clr = ui::color(ui::style::color::group_background))
{
	if (tracking)
	{
		const auto clr = selected
			                 ? ui::color(ui::style::color::view_selected_background)
			                 : bg_clr.average(ui::color(ui::style::color::view_text));

		return clr.scale(hover ? 1.22f : 1.0f).aa(0.9f);
	}

	if (selected)
	{
		const auto clr = view_has_focus
			                 ? ui::color(ui::style::color::view_selected_background).scale(hover ? 1.22f : 1.0f)
			                 : bg_clr.average(ui::color(ui::style::color::view_selected_background)).scale(
				                 hover ? 1.33f : 1.0f);
		return clr.aa(0.9f);
	}

	if (hover)
	{
		const auto clr = bg_clr.average(ui::color(ui::style::color::view_text));
		return text_over ? clr.scale(0.8f).aa(0.9f) : clr.aa(0.9f);
	}

	return bg_clr.scale(text_over ? 1.22f : 1.44f).aa(0.9f);
}

// Every draggable splitter draws through this so the handle and its hover and drag highlights
// are identical wherever a pane can be resized.
inline void draw_splitter_handle(ui::draw_context& dc, const recti bounds, const int splitter_width,
                                 const bool active, const bool tracking)
{
	const auto scale1 = df::round(1 * dc.scale_factor);
	const auto visual_width = std::max(scale1, splitter_width);
	const auto visual_left = bounds.left + (bounds.width() - visual_width) / 2;
	const auto handle_margin = tracking || active ? scale1 : std::max(scale1, df::mul_div(visual_width, 2, 9));

	recti draw_bounds;
	draw_bounds.left = visual_left + handle_margin;
	draw_bounds.right = visual_left + visual_width - handle_margin;
	draw_bounds.top = bounds.top + dc.handle_cxy;
	draw_bounds.bottom = bounds.bottom - dc.handle_cxy;

	if (draw_bounds.height() > 8)
	{
		dc.draw_rounded_rect(draw_bounds, ui::color(0x000000, dc.colors.alpha * dc.colors.bg_alpha), dc.padding1);
	}

	if (active)
	{
		const auto clr = view_handle_color(false, active, tracking, dc.frame_has_focus, false).aa(dc.colors.alpha);
		dc.draw_rounded_rect(draw_bounds.inflate(-scale1), clr, dc.padding1);
	}
}

class view_controller
{
public:
	view_controller(view_host_ptr host, const recti bounds) : _host(std::move(host)), _bounds(bounds)
	{
	}

	virtual ~view_controller() = default;

	view_host_ptr _host;
	// The controller's own region: what it paints over, and what a release has to land in for the
	// gesture to count. Never the region the host caches it for -- that is hit_test_context's job,
	// and clipping this to it silently drops clicks and hover highlights on a partly covered target.
	recti _bounds;
	pointi _last_loc;
	pointi _start_loc;
	float _alpha = 1.0f;

	const recti bounds() const
	{
		return _bounds;
	}

	virtual void draw(ui::draw_context& rc)
	{
	}

	virtual ui::style::cursor cursor() const
	{
		return ui::style::cursor::normal;
	}

	virtual void on_mouse_left_button_down(const pointi loc, const ui::key_state keys)
	{
		_last_loc = _start_loc = loc;
	}

	virtual void on_mouse_move(const pointi loc)
	{
	}

	virtual void on_mouse_left_button_up(const pointi loc, const ui::key_state keys)
	{
	}

	virtual void on_mouse_middle_button_down(const pointi loc, const ui::key_state keys)
	{
	}

	virtual void on_mouse_middle_button_up(const pointi loc, const ui::key_state keys)
	{
	}

	virtual void on_mouse_left_button_double_click(const pointi loc, const ui::key_state keys)
	{
	}

	virtual bool key_down(const char32_t key, const ui::key_state keys)
	{
		return false;
	}

	virtual void on_mouse_leave()
	{
	}

	// True only when something in progress was actually cancelled. Escape is shared with the view
	// and the app, so a controller that merely sits under the pointer must not consume it.
	virtual bool escape()
	{
		return false;
	}

	virtual void popup_from_location(view_hover_element& hover)
	{
	}

	virtual void tick()
	{
	}
};

class view_host_base
{
public:
	virtual void invalidate_element(const view_element_ptr& e) = 0;
	virtual void invalidate_view(view_invalid invalid) = 0;
};

class view_host : public ui::frame_host, public view_host_base
{
public:
	sizei _extent;

	view_controller_ptr _active_controller;

	bool _show_cursor = true;
	bool _hover = false;
	bool _tracking = false;
	bool _controller_invalid = false;
	recti _controller_bounds;
	recti _tooltip_bounds;

	ui::style::cursor _cursor = ui::style::cursor::normal;

	void invalidate_view(view_invalid invalid) override = 0;
	virtual void invoke(commands cmd) = 0;
	virtual bool is_command_checked(commands cmd) = 0;
	virtual void track_menu(recti recti, const std::vector<ui::command_ptr>& commands) = 0;
	virtual void controller_changed() = 0;
	// Never null: a host with no window yet, or none any more, answers ui::no_frame(). Everything
	// below dereferences this without checking, and so may every caller.
	virtual const ui::frame_ptr frame() const = 0;
	virtual const ui::control_frame_ptr owner() = 0;
	virtual view_controller_ptr controller_from_location(pointi loc, hit_test_context& ctx) = 0;

	virtual void scroll_controls()
	{
	}

	void on_window_destroy() override
	{
		_active_controller.reset();
	}

	void update_controller(const pointi loc)
	{
		if (!_tooltip_bounds.is_empty() && !_tooltip_bounds.contains(loc))
		{
			_tooltip_bounds.clear();
			invalidate_view(view_invalid::tooltip);
		}

		if (!_tracking && (_controller_invalid || !_controller_bounds.contains(loc)))
		{
			_active_controller.reset();

			hit_test_context ctx{loc, recti(_extent)};
			_active_controller = controller_from_location(loc, ctx);

			auto cursor = ui::style::cursor::normal;

			if (_active_controller)
			{
				_active_controller->on_mouse_move(loc);
				cursor = _active_controller->cursor();
			}

			_cursor = cursor;
			// The controller answers for its own region; the hit test answers for how far that answer
			// travels. An empty rect here is the idiom for re-testing on every move, which is what a
			// controller standing in for whatever happens to be under the pointer wants.
			_controller_bounds = _active_controller
				                     ? _active_controller->bounds().intersection(ctx.stability())
				                     : recti{};
			_controller_invalid = false;

			controller_changed();
			frame()->invalidate();
		}
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		update_controller(loc);
		update_tracking(loc, true);

		const auto c = _active_controller;

		if (c)
		{
			c->on_mouse_left_button_down(loc, keys);
		}

		update_cursor();
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		update_tracking(loc, false);

		// The copy keeps the controller alive for the duration of its own handler, and is released
		// before the controller is re-tested: a copy still held there would destroy the outgoing
		// controller after its replacement was built, so the replacement's hover state was undone.
		if (const auto c = _active_controller)
		{
			c->on_mouse_left_button_up(loc, keys);
		}

		// on_mouse_left_button_up sometimes shows a dialog
		// it is best to fetch the current cursor location
		update_controller(frame()->cursor_location());
	}

	void on_mouse_middle_button_down(const pointi loc, const ui::key_state keys) override
	{
		update_controller(loc);
		if (_active_controller) _active_controller->on_mouse_middle_button_down(loc, keys);
		update_cursor();
	}

	void on_mouse_middle_button_up(const pointi loc, const ui::key_state keys) override
	{
		if (_active_controller) _active_controller->on_mouse_middle_button_up(loc, keys);
	}

	void on_mouse_left_button_double_click(const pointi loc, const ui::key_state keys) override
	{
		update_controller(loc);
		const auto c = _active_controller;

		if (c)
		{
			c->on_mouse_left_button_double_click(loc, keys);
		}
	}

	void update_tracking(const pointi loc, const bool is_tracking)
	{
		_hover = recti(_extent).contains(loc);
		_tracking = is_tracking;
	}

	bool escape_controller()
	{
		if (!_active_controller) return false;
		if (!_active_controller->escape()) return false;

		if (_tracking)
		{
			// The button is still down -- _tracking is what says so -- and the cancelled controller is
			// the one that must receive its own release. Replacing it here handed that release to a
			// fresh controller with no memory of the cancel, which then performed the very gesture
			// Escape had just refused: a rubber-band selection cancelled mid-drag was replaced on
			// release by the single item under the pointer. The re-test happens once the button is up.
			_controller_invalid = true;
		}
		update_cursor();
		return true;
	}

	bool key_down_controller(const char32_t key, const ui::key_state keys) const
	{
		return _tracking && _active_controller && _active_controller->key_down(key, keys);
	}

	void on_mouse_move(const pointi loc, const bool is_tracking) override
	{
		update_tracking(loc, is_tracking);
		update_controller(loc);

		const auto c = _active_controller;

		if (c)
		{
			c->on_mouse_move(loc);
		}

		update_cursor();
	}

	void on_mouse_leave(const pointi loc) override
	{
		update_controller({-1, -1});
		_hover = false;
	}

	void update_cursor()
	{
		if (_active_controller)
		{
			_cursor = _active_controller->cursor();
		}

		frame()->set_cursor(_show_cursor ? _cursor : ui::style::cursor::none);
	}

	void show_cursor(const bool show)
	{
		if (_show_cursor != show)
		{
			_show_cursor = show;
			update_cursor();
		}
	}

	bool show_cursor() const
	{
		return _show_cursor;
	}
};

class view_base : public df::no_copy
{
public:
	struct progress_state
	{
		bool active = false;
		int64_t position = 0;
		int64_t total = 0;
	};

	bool _view_has_focus = false;
	bool _show_render_window = true;

	view_base() = default;
	~view_base() override = default;

	virtual void activate(sizei extent) = 0;
	virtual void deactivate() = 0;
	virtual void refresh() = 0;

	virtual void refresh_from_source()
	{
		refresh();
	}

	virtual void reload()
	{
	};

	virtual void update_media_elements()
	{
	}

	virtual view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                                     hit_test_context& ctx)
	{
		return nullptr;
	}

	virtual bool can_exit()
	{
		return true;
	}

	virtual bool can_scroll(const pointi loc) const
	{
		return false;
	}

	virtual menu_type context_menu(const pointi loc)
	{
		return menu_type::view;
	}

	// Files dropped on this view, in view coordinates. A view that answers true has consumed the
	// drop; the default is false, which leaves the frame's own handling - opening the first item, or
	// copying into the folder being shown - alone.
	virtual bool drop_paths(const std::vector<df::file_path>& paths, const pointi loc)
	{
		return false;
	}

	// Files from outside are being dragged over this view at `loc`, in view coordinates, or have left
	// it, ended or dropped when empty. A view that inserts a drop at a position shows that position
	// while the drag lasts.
	virtual void drag_paths_over(const std::optional<pointi> loc)
	{
	}

	virtual bool escape()
	{
		return false;
	}

	virtual void render(ui::draw_context& rc, view_controller_ptr controller)
	{
	}

	// Events
	virtual void layout(ui::measure_context& mc, const sizei extent)
	{
	}

	// One entry point for both wheel axes. The pointer chooses the surface and the modifier chooses
	// which of that surface's axes moves; a second virtual for the horizontal axis was a half every
	// view forgot to write. Returns whether the notch meant anything here, so an unconsumed one can
	// fall through instead of being silently swallowed.
	virtual bool mouse_wheel(const pointi loc, const ui::wheel_notch notch)
	{
		return false;
	}

	// Whole detents of a pinch, positive to magnify.
	virtual bool pinch(const pointi loc, const int steps)
	{
		return false;
	}

	// Raised before a left button press is dispatched to a controller, so a view can release text
	// focus the press did not land in. Keyboard commands are inert while a rendered edit holds
	// focus, so that focus must not survive a press somewhere else in the view.
	virtual void mouse_down(const pointi loc)
	{
	}

	virtual bool key_down(char32_t key, ui::key_state keys)
	{
		return false;
	}

	virtual bool text_input(std::string_view text)
	{
		return false;
	}

	virtual ui::focus_mode focus_mode() const
	{
		return ui::focus_mode::view;
	}

	virtual bool is_caption_area(const pointi loc) const
	{
		return false;
	}

	virtual void pan_start(const pointi start_loc)
	{
	}

	virtual void pan(const pointi start_loc, const pointi current_loc)
	{
	}

	virtual void pan_end(const pointi start_loc, const pointi final_loc)
	{
	}

	// Returns false when the tap has no touch-specific meaning, so the caller runs the normal double-click.
	virtual bool touch_double_tap(const pointi location)
	{
		return false;
	}

	virtual void focus(const bool has_focus)
	{
		_view_has_focus = has_focus;
	}

	virtual void items_changed(bool path_changed)
	{
	}

	virtual void display_changed()
	{
	}

	virtual bool is_over_items(const pointi loc) const
	{
		return false;
	}

	virtual void broadcast_event(const view_element_event& event) const
	{
	}

	virtual void exit()
	{
	}

	// Stops any task the view is running and leaves the view open. Distinct from exit().
	virtual void cancel_operation()
	{
	}

	// Human name of the task this view runs, used when reporting that it is still running.
	virtual std::string_view operation_name() const
	{
		return {};
	}

	// Asked before the application closes. Unlike can_exit() this may prompt the user.
	virtual bool confirm_exit()
	{
		return can_exit();
	}

	virtual std::string_view title()
	{
		return {};
	}

	virtual std::string_view status()
	{
		return {};
	}

	virtual progress_state progress() const
	{
		return {};
	}
};

struct view_scroller_section
{
	std::string text;
	icon_index icon = icon_index::none;
	int y = 0;
};

struct view_scroll_anchor
{
	view_element_ptr element;
	int device_top = 0;
	double scroll_ratio = 0.0;
	bool was_at_start = true;
	bool valid = false;
};

class view_scroller
{
public:
	pointi _offset;
	sizei _scroll_extent;
	recti _client_bounds;
	recti _scroll_bounds;
	// The base of the column handed to an action by layout_with_footer. Held here so a scroll
	// repaints the whole column, including an action whose appearance depends on the position.
	recti _footer_bounds;
	std::vector<view_scroller_section> _sections;
	std::function<void(view_hover_element&, pointi)> popup_func;
	std::function<void()> changed_func;

	bool _active = false;
	bool _tracking = false;
	bool _scroll_child_controls = false;

	const recti scroll_bounds() const
	{
		return _scroll_bounds;
	}

	// The painted track. Every mapping between list position and scrollbar position goes through
	// it, so the thumb, the section bands and the hit tests cannot disagree -- in particular once
	// layout_with_footer has given the base of the column away.
	recti track_bounds() const
	{
		constexpr auto inset = 2;
		return _scroll_bounds.height() > inset * 2 ? _scroll_bounds.inflate(0, -inset) : _scroll_bounds;
	}

	// Shared so anything drawn in line with the track sits in the same column as it.
	int track_inset() const
	{
		return !_sections.empty() || _active ? 1 : df::mul_div(_scroll_bounds.width(), 2, 9);
	}

	// Section bands in track-relative coordinates, in paint order.
	template <class F>
	void for_each_band(F&& f) const
	{
		const auto track_height = track_bounds().height();
		if (track_height <= 0) return;

		if (_sections.empty())
		{
			f(0, track_height, static_cast<const view_scroller_section*>(nullptr));
			return;
		}

		constexpr auto band_gap = 2;
		// Matches the minimum the sections are generated against. A band below it cannot be read
		// or aimed at, and drawn against the band above it it reads as a second overlapping band.
		constexpr auto min_band_height = 8;

		const auto count = _sections.size();
		auto top = 0;

		for (size_t i = 0; i < count; ++i)
		{
			const auto& so = _sections[i];
			// Not std::clamp: top can already have run past the track, and clamp with lo > hi is
			// undefined.
			auto bottom = std::min(std::max(logical_to_scrollbar_pos(so.y), top), track_height);

			// The list runs past its last group -- footer, actions, trailing space -- so the track
			// is closed off rather than stopping short of the position the thumb can reach. What is
			// left over only earns its own band if it is big enough to be one; otherwise the last
			// section keeps it.
			if (i + 1 == count && track_height - bottom - band_gap < min_band_height)
			{
				bottom = track_height;
			}

			if (bottom > top) f(top, bottom, &so);
			top = bottom + band_gap;
		}

		if (track_height - top >= min_band_height)
		{
			f(top, track_height, static_cast<const view_scroller_section*>(nullptr));
		}
	}

	// Device y the thumb starts at before it is padded out to a usable size. A drag measures its
	// grab from here so scrollbar_pos_to_logical inverts it exactly.
	int thumb_origin() const
	{
		return track_bounds().top + logical_to_scrollbar_pos(_offset.y);
	}

	// The painted thumb. Hit testing shares it so a press lands on what the user aimed at.
	recti thumb_bounds() const
	{
		const auto track = track_bounds();
		if (!can_scroll() || track.height() <= 0) return {};

		const auto cy = df::mul_div(track.height(), _client_bounds.height(), _scroll_extent.cy);
		const auto pad = cy >= 20 ? 1 : 10;
		const auto origin = thumb_origin();
		const auto inset = track_inset() + 1;

		return {
			track.left + inset, std::clamp(origin - pad, track.top, track.bottom),
			track.right - inset, std::clamp(origin + cy + pad, track.top, track.bottom)
		};
	}

	void scroll_offset(const view_host_ptr& host, const int x, const int y)
	{
		const auto cx = _scroll_extent.cx - _client_bounds.width();
		const auto cy = _scroll_extent.cy - _client_bounds.height();
		const auto xx = cx > 0 ? std::clamp(x, 0, cx) : 0;
		const auto yy = cy > 0 ? std::clamp(y, 0, cy) : 0;

		if ((_offset.x != xx || _offset.y != yy) && _scroll_extent.cy > 0)
		{
			const auto delta = _offset.y - yy;
			_offset.x = xx;
			_offset.y = yy;

			if (delta != 0)
			{
				host->frame()->scroll(0, delta, _client_bounds, _scroll_child_controls);
			}

			host->frame()->invalidate(_footer_bounds.is_empty()
				                          ? _scroll_bounds
				                          : _scroll_bounds.make_union(_footer_bounds));

			if (changed_func)
			{
				changed_func();
			}
		}
	}

	void layout(const sizei scroll_extent, const recti client_bounds, const recti scroll_bounds)
	{
		_scroll_extent = scroll_extent;
		_client_bounds = client_bounds;
		_scroll_bounds = scroll_bounds;

		const auto cx = _scroll_extent.cx - _client_bounds.width();
		const auto cy = _scroll_extent.cy - _client_bounds.height();

		_offset.x = cx > 0 ? std::clamp(_offset.x, 0, cx) : 0;
		_offset.y = cy > 0 ? std::clamp(_offset.y, 0, cy) : 0;
	}

	recti layout_with_footer(sizei scroll_extent, recti client_bounds, recti scroll_bounds,
	                         int footer_extent, int gap);

	void sections(std::vector<view_scroller_section> section_offsets)
	{
		_sections = std::move(section_offsets);
	}

	const pointi scroll_offset() const
	{
		return _offset;
	}

	const recti client_bounds() const
	{
		return _client_bounds;
	}

	view_scroll_anchor capture_anchor(const view_element_ptr& element) const;
	int anchor_offset(const view_scroll_anchor& anchor, bool element_is_current) const;
	void restore_anchor(const view_host_ptr& host, const view_scroll_anchor& anchor, bool element_is_current);

	void reset()
	{
		_offset.x = 0;
		_offset.y = 0;
	}

	// Centres the thumb on a device y. Takes a device y so the caller never has to know where the
	// track begins or how much of the column an action at its base has taken.
	void scrollbar_to(const view_host_ptr& host, const int device_y)
	{
		const auto track = track_bounds();
		if (track.height() <= 0 || _scroll_extent.cy <= 0) return;
		const auto half_handle = df::mul_div(_client_bounds.height(), track.height(), _scroll_extent.cy) / 2;
		scroll_offset(host, 0, scrollbar_pos_to_logical(device_y - half_handle));
	}

	// Puts the thumb origin on a device y, so a drag keeps the point it was grabbed by.
	void scrollbar_thumb_to(const view_host_ptr& host, const int device_y)
	{
		if (track_bounds().height() <= 0 || _scroll_extent.cy <= 0) return;
		scroll_offset(host, 0, scrollbar_pos_to_logical(device_y));
	}

	pointi device_to_logical(const pointi loc) const
	{
		return {loc.x, loc.y + _offset.y};
	}

	pointi logical_to_device(const pointi loc) const
	{
		return {loc.x, loc.y - _offset.y};
	}

	recti logical_to_device(const recti bounds) const
	{
		return {{bounds.left, bounds.top - _offset.y}, bounds.extent()};
	}

	void offset(const view_host_ptr& host, const int x, const int y)
	{
		scroll_offset(host, _offset.x + x, _offset.y + y);
	}

	void draw_scroll(ui::draw_context& dc) const;

	void scroll_popup(view_hover_element& hover, const pointi loc) const
	{
		if (popup_func)
		{
			popup_func(hover, loc);
		}
	}

	bool can_scroll() const
	{
		return _scroll_extent.cy > _client_bounds.height() && _client_bounds.height() > 0;
	}

	void clear()
	{
		_offset = {};
		_scroll_extent = {};
		_client_bounds.clear();
		_scroll_bounds.clear();
		_footer_bounds.clear();
		_sections.clear();
		popup_func = {};
	}

	int logical_to_scrollbar_pos(const int y) const
	{
		return _scroll_extent.cy > 0 ? df::mul_div(y, track_bounds().height(), _scroll_extent.cy) : 0;
	}

	int scrollbar_pos_to_logical(const int device_y) const
	{
		const auto track = track_bounds();
		return track.height() > 0 ? df::mul_div(device_y - track.top, _scroll_extent.cy, track.height()) : 0;
	}
};


class scrubber_element;

class scroll_controller final : public view_controller
{
public:
	view_scroller& _parent;
	pointi _start;
	// Where in the thumb the drag took hold, so the list does not jump under the pointer.
	int _grab_offset = 0;


	scroll_controller(const view_host_ptr& host, view_scroller& parent, const recti bounds) :
		view_controller(host, bounds), _parent(parent)
	{
		_parent._active = true;
	}

	~scroll_controller() override
	{
		if (_parent._tracking)
		{
			escape();
		}

		_parent._active = false;
	}

	// The scroll bar is painted by the view that owns it, never by this controller. Splitting the
	// paint between the two made the bar and its section bands vanish whenever the flag and the
	// active controller disagreed.

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::up_down;
	}

	void update_pos(const int y) const
	{
		_parent.scrollbar_thumb_to(_host, y - _grab_offset);
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_last_loc = loc;
		_parent._tracking = true;
		_start = _parent.scroll_offset();

		// Only a press on bare track jumps. A press on the thumb drags it from where it was held.
		const auto thumb = _parent.thumb_bounds();

		if (thumb.is_empty() || loc.y < thumb.top || loc.y >= thumb.bottom)
		{
			_parent.scrollbar_to(_host, loc.y);
		}

		_grab_offset = loc.y - _parent.thumb_origin();
		_host->frame()->invalidate();
	}

	void on_mouse_move(const pointi loc) override
	{
		_last_loc = loc;

		if (_parent._tracking)
		{
			update_pos(loc.y);
		}
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		const auto moved = loc != _last_loc;
		_last_loc = loc;

		// Escape may already have ended the drag and restored the position; do not re-apply it.
		if (_parent._tracking)
		{
			if (moved) update_pos(loc.y);
			_parent._tracking = false;
		}

		_host->frame()->invalidate();
	}

	bool escape() override
	{
		if (!_parent._tracking) return false;
		_parent.scroll_offset(_host, _start.x, _start.y);
		_parent._tracking = false;
		return true;
	}

	void popup_from_location(view_hover_element& hover) override
	{
		if (_parent._active)
		{
			_parent.scroll_popup(hover, _last_loc);
		}
	}
};
