// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The application's side of the element tree, which is libdesktop UI's: the names it
// has here, the application's invalidation flags and the host that maps an element's requests onto
// them, the views and what a view declares for the frame's title bar, status bar, context menu and
// keyboard, and the parts the keyboard reaches among drawn elements that are not controls. The drawn
// controls are elements of the tree, so their header comes with it.

#pragma once

#include "libdesktop/ui/controls.h"

class view_host;
class view_state;
enum class render_valid;

// The element tree is libdesktop's UI element tree (libdesktop/ui/elements.h); these are the names it has here.
using view_element = ui::element;
using view_element_ptr = ui::element_ptr;
using view_element_style = ui::element_style;
using view_element_padding = ui::element_padding;
using view_element_options = ui::element_options;
using view_element_event_type = ui::element_event_type;
using view_element_event = ui::element_event;
using view_controller = ui::controller;
using view_controller_ptr = ui::controller_ptr;
using view_host_base = ui::element_host_base;
using view_host_base_ptr = ui::element_host_base_ptr;
using view_host_ptr = ui::element_host_ptr;
using view_hover_element = ui::hover_bubble;
using view_scroller = ui::scroller;
using view_scroller_section = ui::scroller_section;
using view_scroll_anchor = ui::scroll_anchor;
using ui::hit_test_context;
using ui::interaction_context;
using ui::scroll_controller;
using ui::clickable_controller;
using ui::default_controller_from_location;
using ui::flex_direction;
using ui::flex_wrap;
using ui::flex_justify;
using ui::flex_align;
using ui::flex_item_layout;
using ui::flex_container_layout;
using ui::flex_layout_result;
namespace flex_item = ui::flex_item;
using ui::calc_flex_layout;
using ui::layout_flex_elements;
using ui::text_element_base;
using ui::text_element;
using ui::link_element;
using ui::divider_element;
using ui::padding_element;
using ui::icon_to_utf8;
using ui::make_icon_element;
using ui::make_icon_link_element;
using ui::link_foreground_color;
using ui::draw_splitter_handle;

inline core::color view_handle_color(const bool selected, const bool hover, const bool tracking,
                                     const bool view_has_focus, const bool text_over,
                                     const core::color bg_clr = core::color(ui::style::colors.group_background))
{
	return ui::handle_color(selected, hover, tracking, view_has_focus, text_over, bg_clr);
}

inline void xdraw_icon(render::draw_context& dc, const icon_index i, const recti bounds, const core::color c,
                       const core::color bg, const render::font_face font = render::font_face::icons)
{
	ui::draw_icon(dc, i, bounds, c, bg, font);
}

// The sets of radio buttons a panel shows side by side, each checking one of its own: where to scan, what
// to write and what to do with a name that is taken.
constexpr int radio_group_scope = 1;
constexpr int radio_group_format = 2;
constexpr int radio_group_collision = 3;

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
	visual_options = view_layout | group_layout | app_layout | options_save | command_state | tooltip | address |
	                 font_size | media_elements,

	options = view_layout | group_layout | app_layout | options_save | sidebar | command_state | tooltip |
	          refresh_items | address | font_size,
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

// The application's flags for what an element asks its host to redo. One map, so every host hears
// exactly the flags it heard when elements named them directly.
constexpr view_invalid to_view_invalid(const ui::host_invalid what)
{
	auto result = view_invalid::none;
	if (what && ui::host_invalid::redraw) result |= view_invalid::view_redraw;
	if (what && ui::host_invalid::layout) result |= view_invalid::view_layout;
	if (what && ui::host_invalid::controller) result |= view_invalid::controller;
	if (what && ui::host_invalid::tooltip) result |= view_invalid::tooltip;
	if (what && ui::host_invalid::animations) result |= view_invalid::animations;
	return result;
}

// A host whose requests become the application's invalidation flags. Every host the application
// makes derives from it, so an element's request reaches the same work its own flags would.
class view_host : public ui::element_host
{
public:
	virtual void invalidate_view(view_invalid invalid) = 0;

	void invalidate(ui::host_invalid what) final { invalidate_view(to_view_invalid(what)); }
};

// The controls a bar can carry that are not toolbar buttons. The frame owns one of each.
enum class bar_control
{
	item_filter, // the box that filters the items by name
	item_size,   // the thumbnail size slider
	item_totals, // the item count and size; hovering shows the breakdown by type
};

// A command, shown as a toolbar button, or one of the frame's own bar controls.
using bar_entry = std::variant<commands, bar_control>;

// What a view shows in the frame's two bars. A view only names the entries; the frame builds the
// controls once, keeps them current and lays them out. `top` and `bottom` are right-aligned: the top
// set beside the title, the bottom set at the end of the status bar. `bottom_leading` starts the status
// bar from its other end, and the view's status takes the room between the two. A running task's
// status bar offers Cancel alone, whatever its view declares.
struct view_bars
{
	// Items is the browser, so its title bar is the navigation bar - the address box between the
	// history and folder buttons, then the window buttons - and `top` is not consulted.
	bool navigation = false;
	std::vector<bar_entry> top;
	std::vector<bar_entry> bottom;
	// Given way to first when the status bar is short: the view's own commands at the trailing end
	// keep their room.
	std::vector<bar_entry> bottom_leading;
};

// The bars every task view shows: the window buttons and Close above, the task's own commands below.
view_bars task_view_bars(std::vector<bar_entry> bottom);

// What a right-click on a view offers. As with its bars, a view only names the commands and the frame builds
// the menu: `row` opens it as a row of buttons - the commonest commands on what the menu acts on, each named
// under its icon - and `list` follows, commands::none between its groups. A view that names nothing leaves
// the frame's own menu.
struct view_menu
{
	std::vector<commands> row;
	std::vector<commands> list;

	bool empty() const { return row.empty() && list.empty(); }
};

// The menu on the item a view shows, as Items' media pane and the media view show it.
const view_menu& media_view_menu();

// The keyboard among drawn elements that are not controls. An element that acts when clicked is a part the
// keyboard can be on, and one that acts on several parts of itself - the stars of a rating, a monitor's buttons -
// names them as a keyboard_parts_source. Tab reaches a group of parts as one stop, the arrows move between them,
// and Enter or Space presses the one the keyboard is on as a click at its centre does, through the controller the
// pointer gets there, so a part does from the keyboard just what it does when clicked.
class keyboard_parts_source
{
public:
	virtual ~keyboard_parts_source() = default;

	// Where each part is, in the element's content coordinates, in the order the keyboard steps through them. None
	// for an element that is only pointed into - a picture, a slider - which the keyboard passes over.
	virtual std::vector<recti> keyboard_parts() const = 0;

	// Presses part index as a click on it does, for an element that finds what a click is on from what it drew
	// rather than from a rectangle. False to have the part clicked at its centre.
	virtual bool press_keyboard_part(const int /*index*/, const view_host_ptr& /*host*/) { return false; }
};

// What the keyboard passes over: an element only pointed into - a picture, a slider, a chart - where what a click
// does depends on the point, and has a key of its own or another way in.
class no_keyboard_parts : public keyboard_parts_source
{
public:
	std::vector<recti> keyboard_parts() const override { return {}; }
};

// One part the keyboard can be on: which part of which element, and where it is drawn in its frame.
struct keyboard_part
{
	view_element_ptr element;
	int index = 0;
	recti bounds;

	bool is(const keyboard_part& other) const { return element == other.element && index == other.index; }
};

// What the keyboard can be on in what roots hold, drawn with their content coordinates moved by offset, in the
// order they are drawn: each control, which takes the focus itself, and each part of everything else that acts.
// Anything hidden or disabled is passed over with all it holds.
using keyboard_target = std::variant<ui::control_ptr, keyboard_part>;
std::vector<keyboard_target> find_keyboard_targets(const std::vector<view_element_ptr>& roots, pointi offset);
// The parts alone.
std::vector<keyboard_part> find_keyboard_parts(const std::vector<view_element_ptr>& roots, pointi offset);

// The part an arrow key moves the keyboard to from parts[from]: Left and Right to the part before or after it, Up
// and Down to the nearest part above or below it, Home and End to the first and the last. -1 for any other key.
int step_keyboard_part(const std::vector<keyboard_part>& parts, int from, char32_t key);

// Presses a part as a click at its centre does, through the controller host offers there.
void press_keyboard_part(const view_host_ptr& host, const keyboard_part& part);

// The ring around what the keyboard is on, where nothing else shows it: just outside bounds, or inside them for
// what fills its space - a whole view.
void draw_keyboard_ring(render::draw_context& dc, recti bounds, bool inside = false);

// Scrolls what scroller shows so bounds, where it is drawn now, is in view.
void scroll_into_view(view_scroller& scroller, const view_host_ptr& host, recti bounds);

// Where in a view's frame the keyboard can be besides a control, in the order Tab visits them: the view's own
// content, and the groups of drawn parts around it.
enum class keyboard_area
{
	sidebar, // Items' sidebar, before its listing
	view,    // the view's own content
	listing, // the links of Items' listing: its headers, the titles of its groups and its footer
	media,   // the media pane, or the panels over a picture shown full screen
	panel,   // a task's panel, where each link or group of parts is a stop of its own, as each control is
};

// A group of drawn parts a view shows around its own content.
struct keyboard_region
{
	keyboard_area area = keyboard_area::view;
	std::vector<view_element_ptr> roots;
	pointi offset;                     // where the roots' content coordinates are drawn in the frame
	recti clip;                        // where the region is drawn; the ring is kept within it
	std::function<void(recti)> reveal; // scrolls the region so a part, where it is drawn now, is in view
};

class view_base : public core::no_copy
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

	virtual void refresh_from_source() { refresh(); }

	virtual void reload() {};

	virtual void update_media_elements() {}

	virtual view_controller_ptr controller_from_location(const view_host_ptr& /*host*/, const pointi /*loc*/,
	                                                     hit_test_context& /*ctx*/)
	{
		return nullptr;
	}

	virtual bool can_exit() { return true; }

	virtual bool can_scroll(const pointi /*loc*/) const { return false; }

	// What a right-click at loc, in view coordinates, offers. A view with a menu of its own names it there; by
	// default the menu is the commands the view names for its status bar, then Close.
	virtual view_menu context_menu(pointi loc);

	// Files dropped on this view, in view coordinates. A view that answers true has consumed the
	// drop; the default is false, which leaves the frame's own handling - opening the first item, or
	// copying into the folder being shown - alone.
	virtual bool drop_paths(const std::vector<core::file_path>& /*paths*/, const pointi /*loc*/) { return false; }

	// Files from outside are being dragged over this view at `loc`, in view coordinates, or have left
	// it, ended or dropped when empty. A view that inserts a drop at a position shows that position
	// while the drag lasts.
	virtual void drag_paths_over(const std::optional<pointi> /*loc*/) {}

	virtual bool escape() { return false; }

	// NOLINTNEXTLINE(performance-unnecessary-value-param): the overrides share this signature
	virtual void render(render::draw_context& /*rc*/, view_controller_ptr /*controller*/) {}

	// Events
	virtual void layout(render::measure_context& /*mc*/, const sizei /*extent*/) {}

	// One entry point for both wheel axes. The pointer chooses the surface and the modifier chooses
	// which of that surface's axes moves; a second virtual for the horizontal axis was a half every
	// view forgot to write. Returns whether the notch meant anything here, so an unconsumed one can
	// fall through instead of being silently swallowed.
	virtual bool mouse_wheel(const pointi /*loc*/, const platform::wheel_notch /*notch*/) { return false; }

	// Whole detents of a pinch, positive to magnify.
	virtual bool pinch(const pointi /*loc*/, const int /*steps*/) { return false; }

	// Raised before a left button press is dispatched to a controller, so a view can release text
	// focus the press did not land in. Keyboard commands are inert while a rendered edit holds
	// focus, so that focus must not survive a press somewhere else in the view.
	virtual void mouse_down(const pointi /*loc*/) {}

	virtual bool key_down(char32_t /*key*/, platform::key_state /*keys*/) { return false; }

	// The groups of drawn parts the view shows around its own content, each a stop of the keyboard's own: Items'
	// sidebar, the links of its listing and its media pane. None by default.
	virtual std::vector<keyboard_region> keyboard_regions() { return {}; }

	// Where what the keyboard acts on in the view's own content is, in view coordinates - the focused item, the
	// clip being edited - which the ring goes around and the keyboard's menu opens beside. Empty for the whole view.
	virtual recti keyboard_bounds() const { return {}; }

	virtual bool text_input(std::string_view /*text*/) { return false; }

	virtual ui::focus_mode focus_mode() const { return ui::focus_mode::view; }

	virtual bool is_caption_area(const pointi /*loc*/) const { return false; }

	virtual void pan_start(const pointi /*start_loc*/) {}

	virtual void pan(const pointi /*start_loc*/, const pointi /*current_loc*/) {}

	virtual void pan_end(const pointi /*start_loc*/, const pointi /*final_loc*/) {}

	// Returns false when the tap has no touch-specific meaning, so the caller runs the normal double-click.
	virtual bool touch_double_tap(const pointi /*location*/) { return false; }

	virtual void focus(const bool has_focus) { _view_has_focus = has_focus; }

	virtual void items_changed(bool /*path_changed*/) {}

	virtual void display_changed() {}

	virtual bool is_over_items(const pointi /*loc*/) const { return false; }

	virtual void broadcast_event(const view_element_event& /*event*/) const {}

	virtual void exit() {}

	// Stops any task the view is running and leaves the view open. Distinct from exit().
	virtual void cancel_operation() {}

	// Human name of the task this view runs, used when reporting that it is still running.
	virtual std::string_view operation_name() const { return {}; }

	// Asked before the application closes. Unlike can_exit() this may prompt the user.
	virtual bool confirm_exit() { return can_exit(); }

	virtual std::string_view title() { return {}; }

	virtual std::string_view status() { return {}; }

	virtual progress_state progress() const { return {}; }

	virtual view_bars bars() const { return {}; }
};

class scrubber_element;
