// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Items grid/list view. Displays thumbnails in grid layout, handles
// selection, keyboard navigation, context menus, and item interactions. Also
// builds the selection detail pane, including the verbose metadata block tree.

#include "pch.h"
#include "model.h"
#include "model_index.h"
#include "model_db.h"
#include "ui_view.h"
#include "ui_dialog.h"
#include "ui_controls.h"
#include "view_items.h"
#include "app_sidebar.h"

static int rendered_toolbar_margin(const int scroll_width)
{
	return std::max(4, scroll_width / 3);
}

static int command_bar_height(ui::measure_context& mc)
{
	return mc.text_line_height(ui::style::font_face::dialog) + mc.padding2 * 2;
}

static int rendered_toolbar_button_extent(const int chrome_height, const int margin)
{
	return std::max(1, chrome_height - margin * 2);
}

template <typename Context>
static int rendered_toolbar_button_width(Context& context, const ui::command_ptr& command,
                                         const int button_extent)
{
	if (command->toolbar_text.empty()) return button_extent;
	return button_extent + context.measure_text(command->toolbar_text, ui::style::font_face::dialog,
	                                            ui::style::text_style::single_line, 10000).cx + context.padding2;
}

static void render_toolbar_button(ui::draw_context& dc, const ui::command_ptr& command, const recti bounds,
                                  const bool window_style, const bool pressed = false)
{
	if (pressed || command->checked)
	{
		const auto background = ui::color(ui::style::color::view_selected_background, dc.colors.alpha);
		dc.draw_rounded_rect(bounds, background, dc.padding1);
	}

	const auto alpha = (command->enable ? 1.0f : 0.35f) * dc.colors.alpha;
	const auto color = pressed || command->checked
		                   ? ui::color(ui::style::color::view_text, alpha)
		                   : ui::color(ui::average(ui::style::color::view_text,
		                                           ui::style::color::dialog_selected_background), alpha);
	if (command->icon != icon_index::none)
	{
		auto icon_bounds = bounds;
		if (window_style)
		{
			const auto icon_extent = std::max(1, bounds.height() / 2);
			icon_bounds = center_rect(sizei{icon_extent, icon_extent}, bounds);
		}
		else if (!command->toolbar_text.empty())
		{
			icon_bounds.right = icon_bounds.left + bounds.height();
		}
		const auto font = window_style
			                  ? ui::style::font_face::small_icons
			                  : ui::style::font_face::icons;
		xdraw_icon(dc, command->icon, icon_bounds, color, {}, font);
		if (!command->toolbar_text.empty())
		{
			auto text_bounds = bounds;
			text_bounds.left = icon_bounds.right;
			dc.draw_text(command->toolbar_text, text_bounds.inflate(-dc.padding1, 0),
			             ui::style::font_face::dialog, ui::style::text_style::single_line, color, {});
		}
	}
	else
	{
		const auto& text = command->toolbar_text.empty() ? command->text : command->toolbar_text;
		dc.draw_text(text, bounds.inflate(-dc.padding1, 0), ui::style::font_face::dialog,
		             ui::style::text_style::single_line_center, color, {});
	}
}

// Discussion #251: the menu the always-visible scroll control opens. Every entry but the last is the
// toolbar's own command object, so a second copy of the toolbar cannot disagree with the first about
// what is offered, enabled or ticked. Scroll to top is the control's own action, offered because the
// click no longer performs it, and it comes last, directly below the settings the menu exists for.
std::vector<ui::command_ptr> items_scroll_menu(const view_state& state, const bool at_top,
                                               std::function<void()> scroll_to_top)
{
	std::vector<ui::command_ptr> result;

	// A null entry is the menu's separator, so an unresolved command must be dropped rather than
	// emplaced - it would become a divider instead of a visible failure. That is also why a
	// separator is only emitted where there is already something for it to divide: two adjacent
	// nulls draw as a double rule, and a trailing one as a rule under the last entry.
	const auto add_separator = [&result]
	{
		if (!result.empty() && result.back() != nullptr) result.emplace_back(nullptr);
	};

	const auto add_command = [&result, &state](const commands id)
	{
		if (auto c = state.find_command(id)) result.emplace_back(std::move(c));
	};

	if (const auto group = state.find_command(commands::menu_group_toolbar); group && group->menu)
	{
		for (auto& c : group->menu()) result.emplace_back(std::move(c));
	}

	add_separator();

	for (const auto id : {commands::filter_photos, commands::filter_videos, commands::filter_audio})
	{
		add_command(id);
	}

	add_separator();
	add_command(commands::browse_recursive);

	auto top = std::make_shared<ui::command>();
	top->icon = icon_index::up;
	top->text = tt.tooltip_scroll_to_top;
	top->enable = !at_top;
	top->invoke = std::move(scroll_to_top);
	result.emplace_back(std::move(top));

	return result;
}

// design.md: the base of the item scrollbar is a back-to-top action. It is painted as the foot of
// the same column as the track above it -- same inset, same band -- so the two read as one control
// rather than as a button parked under a scrollbar.
//
// Discussion #251: it is also the one items-view control that is always on screen. The filter
// toolbar at the top scrolls away, so changing a filter over a long listing meant scrolling to the
// top first. Pinning that toolbar would spend vertical space permanently in the view whose whole
// purpose is showing thumbnails; this control is sticky by construction, so nothing has to be.
class scroll_to_top_element final : public std::enable_shared_from_this<scroll_to_top_element>, public view_element
{
	view_scroller& _scroller;
	view_state& _state;
	mutable recti _device_bounds;

public:
	scroll_to_top_element(view_scroller& scroller, view_state& state) :
		view_element(view_element_style::can_invoke | view_element_style::has_tooltip), _scroller(scroller),
		_state(state)
	{
		padding = {0, 0};
	}

	bool is_at_top() const
	{
		return _scroller.scroll_offset().y <= 0;
	}

	recti column_bounds(const pointi element_offset) const
	{
		return bounds.offset(element_offset).inflate(-_scroller.track_inset(), 0);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {width_limit, mc.icon_cxy};
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto r = column_bounds(element_offset);
		_device_bounds = r;
		const auto bg = calc_background_color(dc);
		dc.draw_rounded_rect(r, bg.a > 0.0f ? bg : ui::color(0x000000, dc.colors.alpha * dc.colors.bg_alpha),
		                     dc.padding1);

		// Dimming says the scroll has nothing left to do; removing it would resize the track under
		// the pointer every time the list reached the top. It is a resting state only: the click
		// opens the menu in every scroll position (discussion #251), and a listing opens at the top,
		// so a control that stayed at a quarter alpha under the pointer would read as disabled in
		// exactly the state the view is usually in.
		const auto engaged = is_style_bit_set(view_element_style::hover) ||
			is_style_bit_set(view_element_style::tracking);
		const auto alpha = dc.colors.alpha * (engaged ? 1.0f : is_at_top() ? 0.25f : 0.66f);
		xdraw_icon(dc, icon_index::up, r, ui::color(dc.colors.foreground, alpha), {});
	}

	// What the listing holds, and - when a filter is hiding anything - how much it is not showing.
	// That second number is the one a user needs before opening the filter menu, not after.
	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		hover.elements->add(make_icon_element(icon_index::up, flex_item::no_break));
		hover.elements->add(std::make_shared<text_element>(
			format_items_totals(_state.summary_shown(), _state.item_index.is_init_complete()),
			ui::style::font_face::dialog, ui::style::text_style::single_line, flex_item::line_break));
		hover.elements->add(std::make_shared<summary_control>(_state.summary_shown(), flex_item::line_break));

		const auto total_count = _state.search_items().size();
		const auto shown_count = _state.display_items().size();

		// The same guard the footer uses: while a search is running the results run ahead of the items
		// added to the listing, and reporting that gap as "filtered out" would be a lie that corrects
		// itself a moment later.
		const auto is_searching = _state.item_index.searching > 0;

		if (!is_searching && total_count > shown_count)
		{
			hover.elements->add(std::make_shared<text_element>(
				str_format(tt.some_items_filtered_fmt.sv(), total_count - shown_count),
				ui::style::font_face::dialog, ui::style::text_style::multiline, flex_item::new_line));
		}

		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type != view_element_event_type::invoke) return;

		// By address: the menu command outlives this call, so a reference to a local reference would
		// dangle the moment the handler returned.
		const auto scroller = &_scroller;
		const auto host = event.host;
		event.host->track_menu(_device_bounds, items_scroll_menu(_state, is_at_top(),
		                                                         [scroller, host]
		                                                         {
			                                                         scroller->scroll_offset(host, 0, 0);
		                                                         }));
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		_device_bounds = column_bounds(element_offset);
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

static view_element_ptr find_center_scroll_element(const std::vector<view_element_ptr>& elements,
                                                   const view_scroller& scroller)
{
	const auto logical_bounds = scroller.client_bounds().offset(0, scroller.scroll_offset().y);
	const auto center_y = logical_bounds.center().y;
	auto closest_distance = INT_MAX;
	view_element_ptr result;

	for (const auto& element : elements)
	{
		if (!element->is_visible() || !element->bounds.intersects(logical_bounds)) continue;
		const auto distance = std::abs(element->bounds.center().y - center_y);
		if (distance < closest_distance)
		{
			closest_distance = distance;
			result = element;
		}
	}

	return result;
}

class preferred_width_element final : public view_element
{
	view_element_ptr _child;
	int _preferred_width = 0;

public:
	preferred_width_element(view_element_ptr child, const int preferred_width) :
		view_element(flex_item::center), _child(std::move(child)), _preferred_width(preferred_width)
	{
	}

	bool is_control_area(const pointi loc, const pointi element_offset) const override
	{
		return _child->is_control_area(loc, element_offset);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return _child->measure(mc, std::min(width_limit, df::round(_preferred_width * mc.scale_factor)));
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;
		_child->layout(mc, bounds, positions);
	}

	void render(ui::draw_context& dc, const pointi offset) const override
	{
		_child->render(dc, offset);
	}

	void hover(interaction_context& ic) override
	{
		_child->hover(ic);
	}

	void dispatch_event(const view_element_event& event) override
	{
		_child->dispatch_event(event);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return _child->controller_from_location(host, loc, element_offset, ctx);
	}
};

class rendered_toolbar_command_element final : public view_element,
                                               public std::enable_shared_from_this<rendered_toolbar_command_element>
{
	commands _id = commands::none;
	ui::command_ptr _command;
	mutable recti _device_bounds;

public:
	rendered_toolbar_command_element(const commands id, ui::command_ptr command) :
		view_element(flex_item::center | view_element_style::can_invoke |
			view_element_style::has_tooltip),
		_id(id), _command(std::move(command))
	{
		margin = {2, 2};
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto margin = rendered_toolbar_margin(mc.scroll_width);
		const auto extent = rendered_toolbar_button_extent(command_bar_height(mc), margin);
		return {std::min(width_limit, rendered_toolbar_button_width(mc, _command, extent)), extent};
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		_device_bounds = bounds.offset(element_offset);
		render_background(dc, element_offset);
		render_toolbar_button(dc, _command, _device_bounds, false);
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		hover.id = _id;
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type != view_element_event_type::invoke || !_command->enable) return;

		if (_command->menu)
		{
			const auto menu = _command->menu();
			if (!menu.empty()) event.host->track_menu(_device_bounds, menu);
		}
		else
		{
			event.host->invoke(_id);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		_device_bounds = bounds.offset(element_offset);
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

// Shared pointer behaviour for the item grid. Both grid gestures track the item under the pointer,
// scrub preview-video frames while hovering and open on double click; only what happens between
// button down and button up differs.
class item_pointer_controller : public view_controller
{
protected:
	items_view& _parent;
	view_scroller& _scroller;
	view_state& _state;
	bool _tracking = false;
	bool _cancel = false;
	pointi _last_logical_loc;
	pointi _start_logical_loc;
	df::item_element_ptr _hover_item;

	item_pointer_controller(const view_host_ptr& host, items_view& parent, view_state& s, view_scroller& scroller,
	                        df::item_element_ptr i, const pointi loc) :
		view_controller(host, {}), _parent(parent), _scroller(scroller), _state(s),
		_last_logical_loc(scroller.device_to_logical(loc)), _start_logical_loc(_last_logical_loc),
		_hover_item(std::move(i))
	{
		if (_hover_item)
		{
			_state.hover_item(_host, _hover_item, true);

			// The item's own region, expanded caption and all. Keeping it inside the grid, and keeping
			// the pin badge reachable, are the hit test's business rather than this constructor's.
			_bounds = _scroller.logical_to_device(_hover_item->interactive_bounds());
		}
	}

	~item_pointer_controller() override
	{
		if (_hover_item)
		{
			_state.hover_item(_host, _hover_item, false);
		}
	}

	// Scrubs the preview frame of a hovered video from the pointer x position.
	void update_hover_preview(const pointi loc) const
	{
		if (_tracking || !_hover_item || !_hover_item->file_type()->has_trait(file_traits::preview_video)) return;

		const auto item_bounds = _hover_item->bounds;

		if (item_bounds.contains(_scroller.device_to_logical(loc)))
		{
			_state.load_hover_thumb(_hover_item, loc.x - item_bounds.left, item_bounds.width());
		}
	}

	void open_item(const df::item_element_ptr& i)
	{
		if (!i) return;

		_state.open(_parent._host, i);

		if (i->file_type()->is_media() && !_state.is_full_screen)
		{
			_state.toggle_full_screen();
		}

		_state.stop_slideshow();
		_cancel = true;
	}

public:
	bool escape() override
	{
		if (!_tracking) return false;
		_cancel = true;
		return true;
	}
};

class item_select_controller final : public item_pointer_controller
{
	bool _selecting = false;

public:
	item_select_controller(const view_host_ptr& host, items_view& parent, view_state& s, view_scroller& scroller,
	                       df::item_element_ptr i, const pointi loc) :
		item_pointer_controller(host, parent, s, scroller, std::move(i), loc)
	{
	}

	~item_select_controller() override
	{
		// Ending clears the highlight bit across every group the selection rectangle touched,
		// so there is no need to walk all items here.
		if (_tracking)
		{
			end_selecting();
		}
	}

	ui::style::cursor cursor() const override
	{
		return _selecting ? ui::style::cursor::select : ui::style::cursor::normal;
	}

	recti calc_logical_selection_bounds() const
	{
		return _selecting ? recti(_start_logical_loc, _last_logical_loc).normalise() : recti();
	}

	void draw(ui::draw_context& rc) override
	{
		if (_selecting)
		{
			// Drawn from the logical rectangle rather than the device one, because auto-scroll moves
			// the content under a stationary pointer: a device-space marquee then sat still over items
			// that were not going to be selected. Clipped to the grid so it never paints over the
			// preview pane or the sidebar.
			const auto selection = _scroller.logical_to_device(calc_logical_selection_bounds())
			                                .intersection(_scroller.client_bounds());
			if (!selection.is_empty())
			{
				rc.draw_rect(selection, ui::color(ui::style::color::dialog_selected_background, 0.5));
			}
		}
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_parent.blur_rendered_filter();
		_tracking = true;
		_cancel = false;
		_last_loc = _start_loc = loc;
		_last_logical_loc = _start_logical_loc = _scroller.device_to_logical(loc);
		update_state();
	}

	void on_mouse_move(const pointi loc) override
	{
		_last_loc = loc;
		update_state();
		update_hover_preview(loc);
	}

	void update_state()
	{
		const auto previous_sel_bounds = calc_logical_selection_bounds();

		const auto start_area = center_rect({8, 8}, _start_loc);
		_selecting = _tracking && !_cancel && !start_area.contains(_last_loc);
		_last_logical_loc = _scroller.device_to_logical(_last_loc);
		apply_highlight(previous_sel_bounds);
	}

	// The highlight bit is transient rubber-band paint that is drawn like selection but is not
	// selection, so the gesture that set it also clears it. Left behind it makes an unselected item
	// keep a selected background that the selection list contradicts and no click can clear.
	void end_selecting()
	{
		const auto previous_sel_bounds = calc_logical_selection_bounds();
		_selecting = false;
		_tracking = false;
		apply_highlight(previous_sel_bounds);
	}

	void apply_highlight(const recti previous_sel_bounds) const
	{
		const auto logical_selection_bounds = calc_logical_selection_bounds();
		const auto changed_bounds = logical_selection_bounds.make_union(previous_sel_bounds);

		for (const auto& group : _state.groups())
		{
			if (group->bounds.intersects(changed_bounds))
			{
				for (const auto& i : group->items())
				{
					i->set_style_bit(view_element_style::highlight, i->bounds.intersects(logical_selection_bounds),
					                 _host, i);
				}
			}
		}

		_host->frame()->invalidate();
	}

	void invoke_select(const ui::key_state keys, const bool perform_open)
	{
		if (_cancel) return;

		if (_selecting)
		{
			_state.select(_parent._host, calc_logical_selection_bounds(), keys.control);
			_state.stop_slideshow();
			return;
		}

		const auto i = _state.item_from_location(_last_logical_loc);

		if (i)
		{
			_state.select(_parent._host, i, keys.control, keys.shift, false);
			_state.stop_slideshow();

			if (perform_open)
			{
				open_item(i);
			}
		}
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		_last_loc = loc;
		update_state();

		invoke_select(keys, false);

		end_selecting();
	}

	void on_mouse_left_button_double_click(const pointi loc, const ui::key_state keys) override
	{
		_last_loc = loc;
		update_state();
		invoke_select(keys, true);
		end_selecting();
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_cancel = true;
		update_state();
		return true;
	}

	void tick() override
	{
		if (_selecting)
		{
			constexpr auto offset = 32;
			const auto client_bounds = _scroller.client_bounds();
			constexpr auto scroll_zone = 64;

			if (_last_loc.y < client_bounds.top + scroll_zone)
			{
				_scroller.offset(_host, 0, -offset);
			}
			else if (_last_loc.y > client_bounds.bottom - scroll_zone)
			{
				_scroller.offset(_host, 0, offset);
			}
			else
			{
				return;
			}

			// The pointer is stationary while the content moves, so the logical end of the rectangle
			// only follows if it is recomputed here. Without this the highlight froze during the
			// scroll and the whole scrolled-past run appeared in the selection at the release.
			update_state();
		}
	}
};

class item_drag_controller final : public item_pointer_controller
{
	bool _drag_started = false;

public:
	item_drag_controller(const view_host_ptr& host, items_view& parent, view_state& s, view_scroller& scroller,
	                     df::item_element_ptr i, const pointi loc) :
		item_pointer_controller(host, parent, s, scroller, std::move(i), loc)
	{
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_parent.blur_rendered_filter();
		_tracking = true;
		_cancel = false;
		_last_loc = _start_loc = loc;
		_last_logical_loc = _start_logical_loc = _scroller.device_to_logical(loc);
		_drag_started = false;
	}

	void on_mouse_move(const pointi loc) override
	{
		_last_loc = loc;

		// Leaving the tile is not enough on its own: the tiles abut, so a press near an edge turned a
		// one-pixel twitch into a drag of the whole selection. The same start area the rubber band
		// uses has to be cleared as well, so one surface does not hold two thresholds.
		if (_tracking && !_cancel && !_bounds.contains(loc) && !center_rect({8, 8}, _start_loc).contains(loc))
		{
			start_drag();
		}
		else
		{
			update_hover_preview(loc);
		}
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		_last_loc = loc;
		_tracking = false;

		if (!_drag_started && !_cancel && _hover_item)
		{
			_state.select(_parent._host, _hover_item, keys.control, keys.shift, false);
			_state.stop_slideshow();
		}
	}

	void on_mouse_left_button_double_click(const pointi loc, const ui::key_state keys) override
	{
		_last_loc = loc;
		open_item(_hover_item);
	}

	void start_drag()
	{
		if (_hover_item)
		{
			df::scope_locked_inc l(df::dragging_items);
			detach_file_handles detach(_state);
			_drag_started = true;
			platform::perform_drag(_host->frame()->handle(), _state.selected_items().file_paths(),
			                       _state.selected_items().folder_paths());
			_tracking = false;
			_state.invalidate_view(view_invalid::controller);
		}
	}
};

void items_view::activate(const sizei extent)
{
	_client_extent = extent;
	_visible_items_valid = false;
	update_media_elements();
}

void items_view::deactivate()
{
	// A view that is not on screen claims no item visibility and queues no thumbnail work.
	// app_frame::tick drives retry_visible_thumbnails whichever view is current, so a retained
	// working set would keep scanning for a grid the user can no longer see.
	for (const auto& i : _visible_items) i.i->is_visible(false);
	_visible_items.clear();
	_draw_items.clear();
	_visible_items_valid = false;
	blur_rendered_filter();
}

void items_view::refresh()
{
	_state.open(_host, _state.search(), {});
}

items_view::items_view(view_state& s, view_host_ptr host) :
	_state(s),
	_host(std::move(host)),
	_sidebar(std::make_shared<sidebar_host>(s))
{
	_filter_edit->text(_state.filter().text());
	_filter_edit->cue = &tt.filter;
	_filter_edit->changed = [this](std::string_view) { apply_rendered_filter(); };
	_filter_edit->request_focus = [this] { focus_rendered_filter(); };
	_filter_edit->focus_changed = [this](const bool focused)
	{
		_host->frame()->invalidate(rendered_filter_bounds());
	};

	_items_scroller.popup_func = [this](view_hover_element& hover, const pointi loc)
	{
		items_scroll_popup(hover, loc);
	};
	_items_scroller.changed_func = [this]
	{
		update_visible_items_list();
		blur_rendered_filter_if_scrolled_away();
	};
	_media_scroller.changed_func = [this] { _state.invalidate_view(view_invalid::view_redraw); };

	_items_scroll_top = std::make_shared<scroll_to_top_element>(_items_scroller, _state);

	_metadata_tree->invalidate = [this]
	{
		_state.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw | view_invalid::controller);
	};
}

items_view::~items_view()
{
	// A listing held by a latched controller can outlive the view; without this its next toggle
	// would call back into a destroyed view.
	_metadata_tree->invalidate = nullptr;
	ui::animations.erase(this);
}

bool items_view::sidebar_visible() const
{
	return setting.show_sidebar && _sidebar_width > 0 && _display && !_display->is_zoom_mode() && !_display->
		comparing();
}

int items_view::content_left() const
{
	return sidebar_visible() ? _sidebar_width + _sidebar_splitter_width : 0;
}

recti items_view::sidebar_bounds() const
{
	return sidebar_visible()
		       ? recti{0, 0, _sidebar_width + _sidebar_splitter_width, _client_extent.cy}
		       : recti{};
}

recti items_view::sidebar_splitter_bounds() const
{
	return sidebar_visible()
		       ? recti{
			       _sidebar_width, 0, _sidebar_width + _sidebar_splitter_width,
			       _sidebar->can_scroll() ? _client_extent.cy / 2 : _client_extent.cy
		       }
		       : recti{};
}

void items_view::sidebar_width(const int x)
{
	const auto min_width = std::max(_scroll_width * 3, df::round(64 * _scale_factor));
	const auto max_width = std::max(min_width, _client_extent.cx / 3);
	const auto width = std::clamp(x, min_width, max_width);
	if (width == _sidebar_width) return;
	setting.sidebar.width = df::round(width / _scale_factor);
	_state.invalidate_view(view_invalid::view_layout);
}

class sidebar_splitter_controller final : public view_controller
{
	items_view& _view;
	int _start_width = 0;
	bool _tracking = false;

public:
	sidebar_splitter_controller(const view_host_ptr& host, items_view& view, const recti bounds) :
		view_controller(host, bounds), _view(view)
	{
		_view._sidebar_splitter_active++;
	}

	~sidebar_splitter_controller() override
	{
		_view._sidebar_splitter_active--;
	}

	void draw(ui::draw_context& dc) override
	{
		_view.draw_splitter(dc, _view.sidebar_splitter_bounds(), true, _tracking);
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::left_right;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_view.blur_rendered_filter();
		_tracking = true;
		_start_width = _view._sidebar_width;
		_view.sidebar_width(loc.x);
	}

	void on_mouse_move(const pointi loc) override
	{
		if (_tracking) _view.sidebar_width(loc.x);
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		if (_tracking) _view.sidebar_width(loc.x);
		_tracking = false;
		// The width is persisted once here rather than on every move, which would write the whole
		// settings store to the registry for each pointer message.
		_view._state.invalidate_view(view_invalid::options_save);
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		_view.sidebar_width(_start_width);
		_view._state.invalidate_view(view_invalid::options_save);
		return true;
	}
};

recti items_view::rendered_filter_bounds() const
{
	// The filter box is laid out inside the list control bar, in item-list logical space.
	return _filter_edit->bounds.offset(-_items_scroller.scroll_offset());
}

void items_view::update_edit_caret()
{
	if (_filter_edit->update_caret()) _host->frame()->invalidate(rendered_filter_bounds());
}

bool items_view::is_caption_area(const pointi loc) const
{
	return false;
}

void items_view::apply_rendered_filter()
{
	_state.filter().wildcard(_filter_edit->text());
	_state.invalidate_view(view_invalid::group_layout | view_invalid::view_redraw);
}

void items_view::focus_rendered_filter()
{
	_host->frame()->focus();
	// The list controls scroll with the list, so bring them back into view before taking focus;
	// typing into a box that is scrolled off screen would be invisible state.
	_items_scroller.scroll_offset(_host, 0, 0);
	_filter_edit->focus();
	_host->frame()->invalidate(rendered_filter_bounds());
}

void items_view::blur_rendered_filter()
{
	// Repaint here rather than at every call site: several of them only release focus and would
	// otherwise leave a caret blinking in a box that no longer has it.
	const auto bounds = rendered_filter_bounds();
	if (_filter_edit->blur()) _host->frame()->invalidate(bounds);
}

// The filter box heads the list and scrolls with it. Focus that has scrolled out of the item
// viewport is hidden state: focus_mode() still reports text_edit, so every key is routed into a box
// the user can no longer see. Hooked to the scroller so the wheel, the scroll bar, the back-to-top
// action and the anchor restore are all covered by one rule.
void items_view::blur_rendered_filter_if_scrolled_away()
{
	if (!_filter_edit->focused()) return;
	const auto bounds = rendered_filter_bounds();
	if (!bounds.is_empty() && bounds.intersects(_regions.items)) return;
	blur_rendered_filter();
}

// Text focus must not survive a press somewhere else in the view. While the filter box holds
// focus, focus_mode() is text_edit and app_frame::key_down routes every key into the box, so a
// press on the sidebar, the preview pane, the control bar or a scroll bar would silently disable
// the whole keyboard until the user clicked a thumbnail.
void items_view::mouse_down(const pointi loc)
{
	if (!_filter_edit->focused()) return;
	if (region_hit(rendered_filter_bounds(), loc)) return;
	blur_rendered_filter();
}

void items_view::focus(const bool has_focus)
{
	view_base::focus(has_focus);

	// Keyboard focus left the render window, so the caret in the filter box is no longer where
	// typing goes. Release it rather than restoring a stale text focus when the window comes back.
	if (!has_focus) blur_rendered_filter();
}

bool items_view::key_down(const char32_t key, const ui::key_state keys)
{
	if (_filter_edit->key_down(key, keys))
	{
		_host->frame()->invalidate(rendered_filter_bounds());
		return true;
	}

	return false;
}

bool items_view::text_input(const std::string_view text)
{
	if (_filter_edit->text_input(text))
	{
		_host->frame()->invalidate(rendered_filter_bounds());
		return true;
	}

	return false;
}

namespace
{
	std::shared_ptr<link_element> make_chip(std::string text, std::function<void()> invoke,
	                                        std::function<void(view_hover_element&)> tooltip, const bool latched)
	{
		auto chip = std::make_shared<link_element>(std::move(text), std::move(invoke), std::move(tooltip),
		                                           ui::style::font_face::dialog,
		                                           ui::style::text_style::multiline_center,
		                                           flex_item::center);
		chip->full_background(true);
		chip->padding = {6, 4};
		chip->margin = {3, 0};
		chip->set_style_bit(view_element_style::selected, latched);
		return chip;
	}

	// locations.md 7.3: a collapsed row still has to say what it is and how much it holds, or the
	// user cannot tell whether opening it is worth the height it will take.
	view_element_ptr make_row_header(const std::string_view title, const uint32_t count,
	                                 std::function<void()> expand)
	{
		auto header = std::make_shared<link_element>(
			std::format("{}|{}", title, str::format_count(count)), std::move(expand),
			ui::style::font_face::dialog, ui::style::text_style::single_line, flex_item::center);
		header->padding = {6, 4};
		return header;
	}
}

// locations.md 7.3: the breakdown row is the only optional row, and it collapses to a one-line
// header when it would otherwise bury the items the user came to see. It never holds a captured
// control while it is asked to collapse.
void items_view::apply_control_row_budget(ui::measure_context& mc, const int width, const int budget)
{
	if (!_breakdown_row.element) return;

	_breakdown_row.collapsed(false);
	const auto natural_height = _breakdown_row.element->measure(mc, width).cy;
	_breakdown_row.collapsed(natural_height > budget && !_breakdown_expanded);
}

// locations.md 7.2: the top places inside the current results. Hidden when the grouping is
// already by location, because the group headers already say this.
// A related search answers about one item, so the view says which item and how that item stands
// against the collection. That explanation used to be reachable only by hovering a badge, where it
// could not be read alongside the answer it belongs to (docs/collections.md section 7).
view_element_ptr items_view::build_related_header_row() const
{
	const auto& search = _state.search();

	if (!search.has_related()) return {};

	const auto& related = search.related();
	const auto name = related.path.name();

	if (name.is_empty()) return {};

	auto row = std::make_shared<view_elements>(flex_item::center);

	auto title = std::make_shared<text_element>(std::format("{} | {}", tt.command_related.sv(), name.sv()),
	                                            ui::style::font_face::dialog,
	                                            ui::style::text_style::single_line_center, flex_item::center);
	title->padding = {6, 4};
	row->add(std::move(title));

	// The anchor is part of its own answer, so its presence is read from the results rather than
	// recomputed here. It stays absent while presence is still resolving.
	for (const auto& item : _state.search_items().items())
	{
		if (item->path() != related.path) continue;
		if (item->presence() == item_presence::unknown) break;

		auto presence = std::make_shared<text_element>(item_presence_text(item->presence(), false),
		                                               ui::style::font_face::dialog,
		                                               ui::style::text_style::single_line_center,
		                                               flex_item::center);
		presence->padding = {6, 4};
		row->add(std::move(presence));
		break;
	}

	return row;
}

view_element_ptr items_view::build_location_breakdown_row()
{
	_breakdown_row.reset();

	const auto& timeline = _state.visits();
	const auto current = _state.search();

	const auto has_location_context = current.has_term_type(df::search_term_type::location) ||
		current.has_term_type(df::search_term_type::area);
	const auto want_places = has_location_context && _state.group_order() != group_by::location &&
		timeline.places.size() > 1;

	if (!want_places) return {};

	auto row = std::make_shared<view_elements>(flex_item::stretch);
	auto total_count = 0u;

	if (want_places)
	{
		for (const auto& place : timeline.places)
		{
			total_count += place.count;
			auto text = std::format("{}|{}", place.name, str::format_count(place.count));

			// The bubble names the place in full, because the chip is only as specific as it can
			// be in the width it has.
			const auto action = df::visit_place_detail(place);

			_breakdown_row.content.emplace_back(make_chip(std::move(text), [this, place]
			                                              {
				                                              _state.open(
					                                              _host, df::visit_place_search(_state.search(), place),
					                                              {});
			                                              }, [action](const view_hover_element& hover)
			                                              {
				                                              hover.elements->add(
					                                              std::make_shared<action_element>(action));
			                                              }, false));
		}
	}

	if (_breakdown_row.content.empty())
	{
		_breakdown_row.reset();
		return {};
	}

	_breakdown_row.header = make_row_header(tt.places_title.sv(), total_count, [this]
	{
		_breakdown_expanded = true;
		_state.invalidate_view(view_invalid::group_layout_complete);
	});

	row->add(_breakdown_row.content);
	row->add(view_element_ptr(_breakdown_row.header));
	_breakdown_row.element = row;
	_breakdown_row.collapsed(false);
	return row;
}

void items_view::items_changed(const bool path_changed)
{
	if (!path_changed)
	{
		const auto focus = _state.focus_item();
		const auto anchor = focus && is_visible(focus) ? focus : _layout_center_item;
		_pending_items_anchor = _items_scroller.capture_anchor(anchor);
	}
	else
	{
		_pending_items_anchor = {};
	}

	if (path_changed)
	{
		_items_scroller.reset();
		_media_scroller.reset();
	}

	std::vector<view_element_ptr> elements;
	elements.reserve(_state.groups().size() * 3 + 2);

	const auto has_items = _state.has_display_items();
	const auto total_count = _state.search_items().size();
	const auto shown_count = _state.display_items().size();

	const auto filter = std::make_shared<view_elements>(flex_item::stretch);
	filter->add(std::make_shared<preferred_width_element>(
		std::make_shared<slider_element>(
			[] { return setting.item_scale_position; },
			[this](const int value)
			{
				setting.set_item_scale_position(value);
				_state.invalidate_view(view_invalid::view_layout);
			},
			0, settings_t::item_scale_position_max, tt.tooltip_thumbnail_size),
		100));
	filter->add(std::make_shared<preferred_width_element>(_filter_edit, 137));
	for (const auto id : {commands::filter_photos, commands::filter_videos, commands::filter_audio})
	{
		filter->add(std::make_shared<rendered_toolbar_command_element>(id, _state.find_command(id)));
	}
	// locations.md 7.1, baseline defect 5: the totals stand on their own. Hovering them shows the
	// breakdown by type; they open nothing, so reaching for the count never opens a grouping menu.
	auto totals = std::make_shared<link_element>(
		format_items_totals(_state.summary_shown(), _state.item_index.is_init_complete()),
		std::function<void()>{},
		[this](const view_hover_element& hover)
		{
			hover.elements->add(std::make_shared<summary_control>(_state.summary_shown(), flex_item::line_break));
		},
		ui::style::font_face::dialog, ui::style::text_style::single_line, flex_item::center);
	totals->foreground_color(0);
	totals->padding = {6, 4};
	filter->add(std::move(totals));

	filter->add(std::make_shared<rendered_toolbar_command_element>(commands::menu_group_toolbar,
	                                                               _state.find_command(commands::menu_group_toolbar)));
	elements.emplace_back(filter);

	if (auto related_row = build_related_header_row())
	{
		elements.emplace_back(std::move(related_row));
	}

	// locations.md 7.3: the breakdown strip sits above the timeline, so the rows read from
	// "where within this" down to "when within this".
	if (auto breakdown_row = build_location_breakdown_row())
	{
		elements.emplace_back(std::move(breakdown_row));
	}

	for (const auto& g : _state.groups())
	{
		elements.emplace_back(build_group_title(_state, _host, g));
		elements.emplace_back(g);
	}

	// The footer is one flex column: the status lines stack and centre, and every action sits in a
	// single wrapping row below them. Gaps do the spacing, so no growing spacer can push the lines
	// apart when the window is taller than the results.
	constexpr auto footer_message_width = 420; // logical units; keeps a message to a readable measure
	std::vector<view_element_ptr> status_elements;
	std::vector<view_element_ptr> action_elements;

	const auto is_searching = _state.item_index.searching > 0;
	const auto has_filtered_out = !is_searching && total_count > shown_count;

	const auto add_status = [&status_elements](const std::string_view text, const ui::style::font_face font)
	{
		auto element = std::make_shared<text_element>(text, font, ui::style::text_style::multiline_center,
		                                              flex_item::center);
		element->flex.max_size.cx = footer_message_width;
		element->padding = {8, 2};
		status_elements.emplace_back(std::move(element));
	};

	const auto add_action = [&action_elements]<typename action_t>(const std::string_view text, action_t action)
	{
		auto link = std::make_shared<link_element>(text, std::move(action), ui::style::font_face::dialog,
		                                           ui::style::text_style::single_line_center,
		                                           flex_item::center, true);
		link->flex.max_size.cx = footer_message_width;
		link->padding = {8, 6};
		action_elements.emplace_back(std::move(link));
	};

	if (is_searching)
	{
		add_status(tt.searching_text, ui::style::font_face::title);
	}
	else if (!has_items && (_state.search().has_selector() || _state.search().has_terms()))
	{
		// Give feedback when a committed search / path resolves to no items, instead of silently
		// showing an empty view. A folder browse just shows "Empty Folder"; an actual search shows
		// "Nothing found" plus guidance on widening the index. A folder emptied by the type filters
		// is not empty, so it says "Nothing found" and leaves the recovery to the filter action.
		const auto browsing_folder = _state.search().is_folder();
		add_status(browsing_folder && !has_filtered_out ? tt.empty_folder : tt.nothing_found1,
		           ui::style::font_face::title);

		if (!browsing_folder && !has_filtered_out)
		{
			// locations.md 3.7: a location query that finds nothing must explain itself and offer
			// the next action. That moment is where trust in the search is won or lost.
			if (const auto* const place_term = _state.search().single_place_term())
			{
				const auto typed = place_term->text;
				const auto km = place_term->float_val;

				add_status(km > 0.0
					           ? str_format(tt.nothing_within_fmt.sv(), format_distance_km(km), typed)
					           : str_format(tt.nothing_at_place_fmt.sv(), typed),
				           ui::style::font_face::dialog);

				if (km > 0.0)
				{
					if (const auto wider = location_distance_at_detent(
						location_nearest_distance_detent(km) + 1); wider > km)
					{
						add_action(str_format(tt.widen_to_fmt.sv(), format_distance_km(wider)),
						           std::function<void()>([this, wider]
						           {
							           auto search = _state.search();
							           search.set_place_distance(wider);
							           _state.open(_host, search, {});
						           }));
					}
				}

				// A qualified name that resolved to the wrong namesake -- offer all of them.
				if (const auto comma = typed.find(','); comma != std::string::npos)
				{
					auto bare = typed.substr(0, comma);
					while (!bare.empty() && bare.back() == ' ') bare.pop_back();

					add_action(str_format(tt.search_all_named_fmt.sv(), bare),
					           std::function<void()>([this, bare]
					           {
						           auto search = _state.search();
						           search.set_place_name(bare);
						           _state.open(_host, search, {});
					           }));
				}

				// So a user never mistakes "not placed" for "not present".
				add_action(tt.show_without_location.sv(),
				           std::function<void()>([this]
				           {
					           auto search = _state.search();
					           search.clear_terms();
					           search.without(df::search_term_type::has_location);
					           _state.open(_host, search, {});
				           }));
			}

			// State the collection's edge, then offer the one move that widens it.
			add_status(tt.nothing_found2, ui::style::font_face::dialog);
			add_action(tt.nothing_found_add_folders.sv(),
			           std::function<void()>([this] { _state.invoke(commands::options_collection); }));
		}
	}

	if (has_filtered_out)
	{
		add_action(str_format(tt.some_items_filtered_fmt.sv(), total_count - shown_count),
		           std::function<void()>([this]
		           {
			           _filter_edit->text({});
			           _state.clear_filters();
		           }));
	}

	if (has_items && is_date_group_order(_state.effective_group_order()))
	{
		add_action(setting.sort_dates_descending
			           ? tt.command_sort_dates_ascending.sv()
			           : tt.command_sort_dates_descending.sv(),
		           setting.sort_dates_descending
			           ? commands::sort_dates_ascending
			           : commands::sort_dates_descending);
	}

	if (!status_elements.empty() || !action_elements.empty())
	{
		if (has_items)
		{
			auto divider = std::make_shared<divider_element>();
			divider->margin = {0, 8};
			elements.emplace_back(std::move(divider));
		}

		auto footer = std::make_shared<view_elements>(flex_item::stretch);
		footer->flex_container.direction = flex_direction::column;
		footer->flex_container.wrap = flex_wrap::no_wrap;
		footer->flex_container.align_items = flex_align::center;
		footer->flex_container.gap = {0, 8};
		footer->flex_container.padding = {8, has_items ? 8 : 32};
		footer->add(status_elements);

		if (!action_elements.empty())
		{
			auto actions = std::make_shared<view_elements>(flex_item::stretch);
			actions->flex_container.justify = flex_justify::center;
			actions->flex_container.gap = {8, 8};
			actions->add(action_elements);
			footer->add(std::move(actions));
		}

		elements.emplace_back(std::move(footer));
	}

	elements.emplace_back(std::make_shared<padding_element>(32));

	std::swap(_item_elements, elements);

	_state.invalidate_view(view_invalid::view_layout);
}

void items_view::draw_splitter(ui::draw_context& dc, const recti bounds, const bool active,
                               const bool tracking) const
{
	draw_splitter_handle(dc, bounds, _sidebar_splitter_width, active, tracking);
}

class splitter_controller final : public view_controller
{
public:
	items_view& _view;
	int _start = 0;
	pointi _start_loc;
	bool _tracking = false;

	splitter_controller(const view_host_ptr& host, items_view& view,
	                    const recti bounds) : view_controller(host, bounds), _view(view)
	{
		_view._splitter_active++;
	}

	~splitter_controller() override
	{
		if (_tracking)
		{
			escape();
		}

		_view._splitter_active--;
	}

	void draw(ui::draw_context& rc) override
	{
		_view.draw_splitter(rc, _view.calc_spliter_bounds(), true, _tracking);
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::left_right;
	}

	void update_pos(const pointi loc) const
	{
		_view.splitter_pos(loc.x, _tracking);
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_tracking = true;
		_start = _view.splitter_pos();
		_start_loc = loc;
		update_pos(loc);
	}

	void on_mouse_move(const pointi loc) override
	{
		if (_tracking)
		{
			update_pos(loc);
		}
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		if (_tracking)
		{
			_tracking = false;
			update_pos(loc);
		}
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		_view.splitter_pos(_start, false);
		return true;
	}
};

// design.md L5: Escape peels one layer. With a rectangle on the picture it clears it; without one it
// falls through to the unwind ladder view_state::escape owns, so there is one order rather than two.
bool items_view::escape()
{
	if (const auto photo = std::dynamic_pointer_cast<photo_control>(_media_element); photo && photo->has_region())
	{
		photo->clear_region();
		return true;
	}

	return false;
}

void items_view::update_regions()
{
	// Single geometry pass. Regions are laid out back to front: the item grid is the background of
	// the view and every piece of chrome that overlaps it is carved out first, so hit testing can
	// simply walk them in priority order.
	_regions = {};
	_regions.sidebar_splitter = sidebar_splitter_bounds();
	_regions.sidebar = sidebar_bounds();

	const auto zoomed = _display && _display->is_zoom_mode();
	const auto comparing = _display && _display->comparing();
	const auto left = content_left();

	if (zoomed || comparing)
	{
		// The media element owns the whole client while zoomed or comparing. The item grid, the
		// splitter and both scroll bars are not drawn, so they must not be hit tested either.
		_regions.media = recti(_client_extent);
		return;
	}

	const auto padding = _scroll_width / 3;
	const auto control_padding = _scroll_width / 2;
	const auto x_max = _client_extent.cx - _scroll_width * 3;
	const auto split_x = splitter_pos();
	constexpr auto top = 0;
	// The item list and the media pane run to the bottom of the client: their controls are drawn
	// inside them rather than in a fixed band, so no height is reserved here.
	const auto bottom = std::max(top, _client_extent.cy);

	_regions.media = {left, top, std::max(left, split_x - padding), bottom};
	_regions.items = {std::min(x_max, split_x + padding), top, _client_extent.cx, bottom};
	_regions.splitter = {
		split_x - control_padding, top, split_x + control_padding,
		_media_scroller.can_scroll() ? bottom / 2 : bottom
	};

	if (_items_scroller.can_scroll())
	{
		_regions.items_scroll = _items_scroller.scroll_bounds();
		_regions.items_scroll_top = _items_scroll_top_bounds;
	}
	if (_media_scroller.can_scroll()) _regions.media_scroll = _media_scroller.scroll_bounds();
}

view_controller_ptr items_view::media_controller_from_location(const view_host_ptr& host, const pointi loc,
                                                               hit_test_context& ctx)
{
	const auto media_offset = -_media_scroller.scroll_offset();

	if (_display && (_display->is_zoom_mode() || _display->comparing()))
	{
		// Only the primary media element was laid out in this mode; the rest of the stack holds
		// bounds from the previous layout and must not answer hit tests.
		return _media_element ? _media_element->controller_from_location(host, loc, media_offset, ctx) : nullptr;
	}

	for (const auto& e : _media_elements)
	{
		if (auto controller = e->controller_from_location(host, loc, media_offset, ctx))
		{
			return controller;
		}
	}

	return nullptr;
}

view_controller_ptr items_view::items_controller_from_location(const view_host_ptr& host, const pointi loc,
                                                               hit_test_context& ctx)
{
	const auto items_offset = -_items_scroller.scroll_offset();

	for (const auto& e : _item_elements)
	{
		if (auto controller = e->controller_from_location(host, loc, items_offset, ctx))
		{
			return controller;
		}
	}

	const auto i = _state.item_from_location(_items_scroller.device_to_logical(loc));

	// The pin badge wins over selection: it is the only way to release the held item from the grid.
	if (i && _state._pin_item == i)
	{
		if (const auto badge = i->pin_badge_bounds().offset(items_offset); ctx.occluded(badge))
		{
			return std::make_shared<unpin_badge_controller>(host, _state, badge);
		}
	}

	if (i && i->is_selected() && _state.can_process_selection(host, df::process_items_type::local_file_or_folder))
	{
		return std::make_shared<item_drag_controller>(host, *this, _state, _items_scroller, i, loc);
	}

	return std::make_shared<item_select_controller>(host, *this, _state, _items_scroller, i, loc);
}

// One ordered walk over the regions calculated by update_regions(). Chrome, scroll bars and
// splitters are all tested before the item grid. Hit testing, wheel routing and the context menu all
// consume this rather than each writing the order out again: they had drifted, and the wheel's copy
// had lost the splitters, both scroll bars and the scroll control entirely.
items_view::view_region items_view::region_at(const pointi loc) const
{
	if (region_hit(_regions.sidebar_splitter, loc)) return view_region::sidebar_splitter;
	if (region_hit(_regions.sidebar, loc)) return view_region::sidebar;
	if (region_hit(_regions.splitter, loc)) return view_region::splitter;
	if (region_hit(_regions.items_scroll, loc)) return view_region::items_scroll;
	if (region_hit(_regions.items_scroll_top, loc)) return view_region::items_scroll_top;
	if (region_hit(_regions.media_scroll, loc)) return view_region::media_scroll;
	if (region_hit(_regions.media, loc)) return view_region::media;
	if (region_hit(_regions.items, loc)) return view_region::items;
	return view_region::none;
}

recti items_view::region_bounds(const view_region region) const
{
	switch (region)
	{
	case view_region::sidebar_splitter: return _regions.sidebar_splitter;
	case view_region::sidebar: return _regions.sidebar;
	case view_region::splitter: return _regions.splitter;
	case view_region::items_scroll: return _regions.items_scroll;
	case view_region::items_scroll_top: return _regions.items_scroll_top;
	case view_region::media_scroll: return _regions.media_scroll;
	case view_region::media: return _regions.media;
	case view_region::items: return _regions.items;
	case view_region::none: break;
	}

	return {};
}

view_controller_ptr items_view::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                         hit_test_context& ctx)
{
	const auto region = region_at(loc);

	// Each controller is confined to the region that produced it: view_host caches the active
	// controller and only re-tests once the pointer leaves it, so a controller that overhangs its
	// region - a hovered item's expanded caption does - would make the neighbouring region unclickable.
	ctx.confine(region_bounds(region));

	switch (region)
	{
	case view_region::sidebar_splitter:
		return std::make_shared<sidebar_splitter_controller>(host, *this, _regions.sidebar_splitter);
	case view_region::sidebar:
		return _sidebar->controller_from_location(loc, ctx);
	case view_region::splitter:
		return std::make_shared<splitter_controller>(host, *this, _regions.splitter);
	case view_region::items_scroll:
		return std::make_shared<scroll_controller>(host, _items_scroller, _regions.items_scroll);
	case view_region::items_scroll_top:
		return _items_scroll_top->controller_from_location(host, loc, {}, ctx);
	case view_region::media_scroll:
		return std::make_shared<scroll_controller>(host, _media_scroller, _regions.media_scroll);
	case view_region::media:
		return media_controller_from_location(host, loc, ctx);
	case view_region::items:
		return items_controller_from_location(host, loc, ctx);
	case view_region::none:
		break;
	}

	return nullptr;
}

menu_type items_view::context_menu(const pointi loc)
{
	// A press that opens a menu is still a press somewhere else in the view, so it releases text
	// focus by the same rule as a left press. Left focused, the box keeps focus_mode() at text_edit
	// and every keyboard command stays dead after the menu closes.
	mouse_down(loc);

	const auto region = region_at(loc);

	if (region == view_region::sidebar || region == view_region::sidebar_splitter)
	{
		return menu_type::sidebar;
	}

	// The scroll bar and the scroll control are drawn inside the grid's rectangle and belong to the
	// listing, so a press on either offers the listing's menu -- the same answer the wheel gives them.
	if (region == view_region::items || region == view_region::items_scroll ||
		region == view_region::items_scroll_top)
	{
		const auto i = _state.item_from_location(_items_scroller.device_to_logical(loc));

		if (i && !i->is_selected())
		{
			_state.select(_host, i, false, false, false);
		}

		return menu_type::items;
	}

	return menu_type::media;
}

// zoom.md 11: the pointer chooses the surface and the modifier chooses which of that surface's axes
// moves. The modifier never changes meaning with position -- only what it acts on -- which is what
// the old chain could not say: a bare Ctrl test sat between two region tests, so Ctrl resized
// thumbnails everywhere except over the preview pane, where it silently did nothing.
bool items_view::mouse_wheel(const pointi loc, const ui::wheel_notch notch)
{
	const df::scope_exit refresh_controller([this] { _state.invalidate_view(view_invalid::controller); });

	switch (region_at(loc))
	{
	case view_region::sidebar:
		if (!notch.is_vertical()) return false;
		_sidebar->_scroller.offset(_sidebar, 0, -notch.delta);
		return true;

	case view_region::media:
	case view_region::media_scroll:
		return media_wheel(loc, notch);

	case view_region::items:
	case view_region::items_scroll:
	case view_region::items_scroll_top:
		// Ctrl is scale, and for a grid of thumbnails scale is the size of a thumbnail.
		if (notch.keys.control)
		{
			if (notch.steps == 0) return true;
			setting.step_item_scale(notch.steps > 0 ? 1 : -1);
			_state.invalidate_view(view_invalid::view_layout);
			return true;
		}

		if (!notch.is_vertical()) return false;
		_items_scroller.offset(_host, 0, -notch.delta);
		return true;

	case view_region::splitter:
	case view_region::sidebar_splitter:
	case view_region::none:
		break;
	}

	return false;
}

// The preview pane has two states and they answer the wheel differently, because what the notch can
// usefully move is different: fitted there is a column of information to read, magnified there is a
// picture to step through and a ladder to climb.
bool items_view::media_wheel(const pointi loc, const ui::wheel_notch notch)
{
	const auto magnified = _display && (_display->is_temporary_zoom() || _display->is_zoom_mode());

	if (magnified)
	{
		// Horizontal is the secondary axis in every magnified state, inspect zoom included: a tilt is
		// not a magnification gesture, and its steps come from the other accumulator anyway.
		if (notch.is_horizontal())
		{
			_display->pan_zoom_by({-notch.delta / 2.0, 0.0});
			return true;
		}

		// Inspect zoom is a gesture the user is already holding, so its wheel adjusts the temporary
		// magnification whatever the modifier says; in durable zoom mode Ctrl is the ladder.
		if (_display->is_temporary_zoom() || notch.keys.control)
		{
			return step_zoom(loc, notch.steps);
		}

		if (notch.delta == 0) return true;
		_state.select_next(_host, notch.delta <= 0, false, notch.keys.shift);
		return true;
	}

	// zoom.md 11: Ctrl over a fitted image is not an entry into zoom mode. It is consumed rather than
	// left to fall through, so the cheapest possible input cannot reach a neighbouring surface's scale.
	if (notch.keys.control) return true;
	if (!notch.is_vertical()) return false;

	// The pane scrolls rather than navigating while there is a column of detail to read: a wheel notch
	// here is a reading gesture over a scrollable column. With nothing to scroll it means what it
	// means everywhere else over a picture.
	if (_media_scroller.can_scroll())
	{
		_media_scroller.offset(_host, 0, -notch.delta);
		return true;
	}

	if (!_display) return false;
	if (notch.delta == 0) return true;
	_state.select_next(_host, notch.delta <= 0, false, notch.keys.shift);
	return true;
}

// A pinch is a deliberate two-finger gesture rather than the one accidental notch zoom.md refuses to
// let into zoom mode, so it is allowed to be the scale control for whatever it is over.
bool items_view::pinch(const pointi loc, const int steps)
{
	const df::scope_exit refresh_controller([this] { _state.invalidate_view(view_invalid::controller); });

	switch (region_at(loc))
	{
	case view_region::media:
	case view_region::media_scroll:
		if (!_display || !_display->can_zoom()) return false;
		return step_zoom(loc, steps);

	case view_region::items:
	case view_region::items_scroll:
	case view_region::items_scroll_top:
		if (steps == 0) return true;
		setting.step_item_scale(steps > 0 ? 1 : -1);
		_state.invalidate_view(view_invalid::view_layout);
		return true;

	default:
		break;
	}

	return false;
}

// Which pane a comparison magnifies is chosen by the pointer, and the ladder anchors within that
// pane. Both take the pointer in client coordinates, because `zoom_layout` records each viewport's
// origin in those: subtracting the media region first counted that origin twice, which was invisible
// in zoom mode -- where the region is the whole client -- and wrong for every inspect zoom in the
// preview pane, where it is not.
bool items_view::step_zoom(const pointi loc, const int steps)
{
	if (steps == 0) return true;

	const auto local = pointd(loc);
	_display->active_zoom_pane_at(local);
	const auto anchor = _display->zoom_anchor_at(local);

	for (auto step = 0; step < std::abs(steps); ++step)
		_display->adjust_zoom_scale(steps > 0 ? 1 : -1, anchor);

	return true;
}

// Hands the visible working set to the surface-staging workers.
static df::item_elements visible_items(const std::vector<item_and_group>& visible)
{
	df::item_elements items;
	items.reserve(visible.size());
	for (const auto& entry : visible) items.emplace_back(entry.i);
	return items;
}

static void queue_stage_thumbnails(index_state& index, df::item_elements items)
{
	if (items.empty()) return;
	index.queue_stage_thumbnails(std::move(items));
}

// Requests whatever thumbnail work the visible items still need. Shared by the immediate path and
// the one that runs once a database thumbnail load has completed.
static void queue_thumbnail_loads(index_state& index, const df::item_elements& visible)
{
	df::item_elements load;
	df::item_elements offline_load;

	for (const auto& item : visible)
	{
		item->add_if_thumbnail_load_needed(load);
		item->add_if_shell_thumbnail_needed(offline_load);
	}

	if (!load.empty()) index.queue_scan_displayed_items(std::move(load));
	if (!offline_load.empty()) index.queue_scan_offline_thumbnails(std::move(offline_load));
}

void items_view::update_visible_items_list()
{
	std::vector<item_and_group> new_visible_items;
	new_visible_items.reserve(_visible_items.size());

	const auto logical_items_bounds = calc_logical_items_bounds();
	const auto extended_logical_items_bounds = logical_items_bounds.inflate(0, logical_items_bounds.height() / 2);
	const auto& groups = _state.groups();
	const auto first_group = std::lower_bound(groups.begin(), groups.end(), extended_logical_items_bounds.top,
	                                          [](const df::item_group_ptr& group, const int top)
	                                          {
		                                          return group->bounds.bottom < top;
	                                          });

	for (auto group_it = first_group;
	     group_it != groups.end() && (*group_it)->bounds.top <= extended_logical_items_bounds.bottom; ++group_it)
	{
		const auto& group = *group_it;
		if (group->bounds.intersects(extended_logical_items_bounds))
		{
			const auto& items = group->items();
			const auto first = std::lower_bound(items.begin(), items.end(), extended_logical_items_bounds.top,
			                                    [](const df::item_element_ptr& item, const int top)
			                                    {
				                                    return item->bounds.bottom < top;
			                                    });

			for (auto it = first; it != items.end() && (*it)->bounds.top <= extended_logical_items_bounds.bottom; ++it)
			{
				if ((*it)->bounds.intersects(extended_logical_items_bounds))
				{
					new_visible_items.emplace_back(group, *it);
				}
			}
		}
	}

	// No sort: groups are walked in display order and each group's items are already ordered by
	// bounds, so the traversal above is the canonical order. Sorting would cost O(n log n) on every
	// scroll step and would make the order depend on pointer addresses.

	if (!_visible_items_valid || _visible_items != new_visible_items)
	{
		// Maintain a per-item visibility flag for the cloud (offline) thumbnail fetcher: an in-flight
		// batch reads it to abandon items that have scrolled out of view. Clear the ones leaving the
		// (extended) viewport, then mark the new set visible.
		for (const auto& i : _visible_items) i.i->is_visible(false);
		for (const auto& i : new_visible_items) i.i->is_visible(true);

		_visible_items = std::move(new_visible_items);
		_visible_items_valid = true;
		stage_visible_thumbnails();
		df::trace("items_view::update_visible_items_list changed");

		const auto visible_center_loc = logical_items_bounds.center();
		auto visible_center_distance = INT_MAX;
		df::item_element_ptr center_element;

		database::thumbnail_requests db_thumbnail_requests;
		df::item_elements resolved_loads;

		// Every entry already intersects the extended bounds by construction.
		for (const auto& i : _visible_items)
		{
			const auto distance_to_center = i.i->bounds.center().dist_sqrd(visible_center_loc);

			if (distance_to_center < visible_center_distance)
			{
				visible_center_distance = distance_to_center;
				center_element = i.i;
			}

			if (const auto generation = i.i->begin_db_thumbnail_query())
			{
				db_thumbnail_requests.emplace_back(database::make_thumbnail_request(i.i, generation));
			}
			else
			{
				resolved_loads.emplace_back(i.i);
			}
		}


		_layout_center_item = center_element;

		if (!db_thumbnail_requests.empty())
		{
			_state._async.queue_database(
				[&s = _state, db_thumbnail_requests = std::move(db_thumbnail_requests)](
				const database& db) mutable
				{
					db.load_thumbnails(s.item_index, db_thumbnail_requests);
					s._async.queue_ui([&s, requests = std::move(db_thumbnail_requests)]
					{
						df::item_elements current_visible;
						current_visible.reserve(requests.size());
						for (const auto& request : requests)
						{
							const auto item = request.lifetime.lock();
							if (item && item->path() == request.path && item->is_visible())
							{
								current_visible.emplace_back(std::move(item));
							}
						}

						queue_thumbnail_loads(s.item_index, current_visible);
						queue_stage_thumbnails(s.item_index, std::move(current_visible));
					});
				});
		}

		// Items whose database query has already run are requested here rather than waiting on the
		// database hop, which only ever resolves the items it was given.
		queue_thumbnail_loads(_state.item_index, resolved_loads);
	}

	// Pick up re-armed cloud thumbnails whenever layout or scrolling changes the working set.
	df::item_elements offline_retry;

	for (const auto& i : _visible_items)
	{
		i.i->add_if_shell_thumbnail_needed(offline_retry);
	}

	if (!offline_retry.empty())
	{
		_state.item_index.queue_scan_offline_thumbnails(std::move(offline_retry));
	}
}

void items_view::retry_visible_thumbnails(const double time_now)
{
	if (time_now < _next_thumbnail_retry) return;
	_next_thumbnail_retry = time_now + 1.5;

	df::item_elements retry;
	for (const auto& entry : _visible_items)
	{
		if (entry.i->shell_thumbnail_retry_pending())
		{
			entry.i->shell_thumbnail_retry_pending(false);
			entry.i->add_if_shell_thumbnail_needed(retry);
		}
	}

	if (!retry.empty())
	{
		_state.item_index.queue_scan_offline_thumbnails(std::move(retry));
	}

	// A cancelled batch abandons every item it had not reached, and the visible set stops changing
	// once scrolling stops. An outstanding claim means a batch still covers these items.
	if (std::ranges::any_of(_visible_items, [](const item_and_group& entry)
	{
		return entry.i->is_loading_thumbnail();
	}))
	{
		return;
	}

	df::item_elements load;
	for (const auto& entry : _visible_items) entry.i->add_if_thumbnail_load_needed(load);
	if (!load.empty()) _state.item_index.queue_scan_displayed_items(std::move(load));
}

void items_view::stage_visible_thumbnails()
{
	auto items = visible_items(_visible_items);

	// The panel draws cover art from the displayed item's staged surface, and after a full release
	// that item may be nowhere near the visible band.
	if (_display)
	{
		for (const auto& i : {_display->_item1, _display->_item2})
		{
			if (i && std::ranges::find(items, i) == items.end()) items.emplace_back(i);
		}
	}

	queue_stage_thumbnails(_state.item_index, std::move(items));
}


void items_view::render(ui::draw_context& dc, const view_controller_ptr controller)
{
	const auto media_offset = -_media_scroller.scroll_offset();

	if (_display) _display->media_offset = media_offset;

	if (_display && (_display->is_zoom_mode() || _display->comparing()))
	{
		if (_media_element)
		{
			const ui::scoped_clip clip(dc, calc_media_bounds());
			_media_element->render(dc, -media_offset);
		}
	}
	else
	{
		const auto items_offset = -_items_scroller.scroll_offset();
		const auto logical_view_bounds = calc_logical_items_bounds();

		_items_scroller.draw_scroll(dc);

		if (!_regions.items_scroll_top.is_empty())
		{
			_items_scroll_top->render(dc, {});
		}

		_media_scroller.draw_scroll(dc);

		if (_splitter_active == 0)
		{
			draw_splitter(dc, _regions.splitter, _splitter_active != 0, false);
		}

		{
			const ui::scoped_clip clip(dc, _regions.items);
			for (const auto& e : _item_elements)
			{
				if (e->bounds.intersects(logical_view_bounds))
				{
					e->render(dc, items_offset);
				}
			}
		}

		{
			const ui::scoped_clip clip(dc, _regions.media);
			for (const auto& e : _media_elements)
			{
				e->render(dc, media_offset);
			}
		}

		{
			const ui::scoped_clip items_clip(dc, _regions.items);
			const auto& state_focus = _state.focus_item();
			const auto has_related = _state.search().has_related();

			item_and_group focus;
			item_and_group hover;
			std::unordered_set<df::item_group_ptr> update_row_layout_groups;

			// One classification pass. _visible_items is deliberately larger than the viewport so
			// thumbnails are staged ahead of scrolling; only the items that actually intersect the
			// viewport are drawn. Focus and hover are drawn last so they paint over their
			// neighbours.
			_draw_items.clear();
			_draw_items.reserve(_visible_items.size());

			for (const auto& ii : _visible_items)
			{
				if (!ii.i->bounds.intersects(logical_view_bounds)) continue;

				if (ii.g->_display == df::item_group_display::detail && !ii.i->row_layout_valid)
				{
					ii.g->update_detail_row_layout(dc, ii.i, has_related);
					update_row_layout_groups.emplace(ii.g);
				}

				if (ii.i == state_focus)
				{
					focus = ii;
				}
				else if (ii.i->is_style_bit_set(view_element_style::hover))
				{
					hover = ii;
				}
				else
				{
					_draw_items.emplace_back(ii);
				}
			}

			for (const auto& g : update_row_layout_groups)
			{
				g->update_row_layout(dc);
			}

			// Backgrounds of the whole set are drawn before any content so that adjacent
			// backgrounds cannot paint over a neighbour's text.
			for (const auto& i : _draw_items)
			{
				i.i->render_bg(dc, *i.g, items_offset);
			}

			for (const auto& i : _draw_items)
			{
				i.i->render(dc, *i.g, items_offset);
			}

			if (focus.i)
			{
				focus.i->render_bg(dc, *focus.g, items_offset);
				focus.i->render(dc, *focus.g, items_offset);
			}

			if (hover.i)
			{
				hover.i->render_bg(dc, *hover.g, items_offset);
				hover.i->render(dc, *hover.g, items_offset);
			}
		}

		if (sidebar_visible())
		{
			_sidebar->render_embedded(dc);
			draw_splitter(dc, _regions.sidebar_splitter, _sidebar_splitter_active != 0, false);
		}
	}

	// Last, and over everything: the address bar above shares this view's background, so without an
	// edge the two read as one surface and it is not obvious which part scrolls.
	dc.draw_rect(recti(0, 0, _client_extent.cx, 1), ui::color(0.0f, 0.0f, 0.0f, dc.colors.alpha * 0.5f));
}

void items_view::layout_chrome(ui::measure_context& mc)
{
	// The list command bar is laid out with the list it heads. All that is decided here is whether
	// it takes part at all: a zoomed or comparing image owns the whole client.
	const auto owns_client = _display && (_display->is_zoom_mode() || _display->comparing());
	_filter_edit->is_visible(!owns_client);
}

void items_view::make_visible(const df::item_element_ptr& i)
{
	if (i)
	{
		const auto scroll_offset = _items_scroller.scroll_offset();
		const auto client_bounds = _items_scroller.client_bounds();
		const auto bounds = i->bounds;
		auto point_offset = scroll_offset;
		const auto logical_top = scroll_offset.y + client_bounds.top;
		const auto logical_bottom = scroll_offset.y + client_bounds.bottom;

		if (bounds.top < logical_top)
		{
			point_offset.y = bounds.top - client_bounds.top;
		}
		else if (bounds.bottom > logical_bottom)
		{
			point_offset.y = bounds.bottom - client_bounds.bottom;
		}

		_items_scroller.scroll_offset(_host, point_offset.x, point_offset.y);
	}
}

bool items_view::is_visible(const df::item_element_ptr& i) const
{
	if (i)
	{
		const auto logical_bounds = calc_logical_items_bounds();
		return i->bounds.intersects(logical_bounds);
	}

	return false;
}

void items_view::update_item_scroller_sections()
{
	std::vector<view_scroller_section> sections;
	const auto& groups = _state.groups();

	if (!groups.empty())
	{
		// A band this short cannot be seen or aimed at. It keeps its space, which folds into the
		// band that follows it; a band too short for its label just goes unlabelled when drawn.
		constexpr auto min_section_height = 8;

		std::string_view run_text = groups.front()->scroll_text;
		auto run_icon = groups.front()->icon;
		auto run_top = groups.front()->bounds.top;

		// A band that swallowed several short runs is named by whichever of them fills most of it.
		// Naming it after the run that happens to end at the boundary let a single-item group
		// caption a stretch of the list it barely appears in.
		auto band_top = 0;
		auto caption_text = run_text;
		auto caption_icon = run_icon;
		auto caption_height = 0;

		const auto close_run = [&](const int run_bottom)
		{
			const auto height = run_bottom - run_top;

			if (height > caption_height)
			{
				caption_height = height;
				caption_text = run_text;
				caption_icon = run_icon;
			}
		};

		for (const auto& group : groups)
		{
			const std::string_view text = group->scroll_text;
			const auto icon = group->icon;
			const auto y = group->bounds.top;

			if (str::icmp(run_text, text) != 0 ||
				run_icon != icon)
			{
				close_run(y);
				run_text = text;
				run_icon = icon;
				run_top = y;

				// band_top only advances on a kept section, so this measures the painted band.
				if (_items_scroller.logical_to_scrollbar_pos(y - band_top) > min_section_height)
				{
					sections.emplace_back(std::string(caption_text), caption_icon, y);
					band_top = y;
					caption_height = 0;
				}
			}
		}

		const auto list_bottom = groups.back()->bounds.bottom;
		close_run(list_bottom);

		if (_items_scroller.logical_to_scrollbar_pos(list_bottom - band_top) > min_section_height)
		{
			sections.emplace_back(std::string(caption_text), caption_icon, list_bottom);
		}
	}

	_items_scroller.sections(std::move(sections));
}

int layout_media_column(const media_column_inputs& in, ui::measure_context& mc, ui::control_layouts& positions)
{
	flex_container_layout media_column;
	media_column.direction = flex_direction::column;
	media_column.wrap = flex_wrap::no_wrap;
	media_column.align_items = flex_align::start;
	media_column.padding.cx = 8;

	if (in.priority->empty())
	{
		// Nothing is held above the fold, so the whole stack is one run.
		const auto extent = calc_flex_layout(*in.all, mc, {in.bounds.width(), -1}, media_column).extent;
		if (extent.cy <= 0) return 0;

		const auto top = std::max(0, in.bounds.height() - extent.cy) / 2;
		const recti all_bounds{
			in.bounds.left, in.bounds.top + top, in.bounds.right, in.bounds.top + top + extent.cy
		};
		layout_flex_elements(*in.all, mc, positions, all_bounds, media_column);
		return top + extent.cy;
	}

	// The picture keeps half the pane. Information past that scrolls instead of being paid for by the
	// media, which is the bound fullscreen puts on its overlay read from the other side. Without it
	// the media was the only thing that ever gave way, down to the 64-unit floor of flex_item::media.
	const auto media = in.media && in.media->flex.shrink > 0.0f ? in.media : nullptr;
	const auto restore_media_min = media ? media->flex.min_size.cy : 0;

	// The element is shared and outlives this call, so the floor has to come back even if measuring
	// or laying out a child throws - otherwise it keeps a raised minimum height for the session.
	const df::scope_exit restore_floor([media, restore_media_min]
	{
		if (media) media->flex.min_size.cy = restore_media_min;
	});

	if (media)
	{
		const auto content_width = std::max(
			0, in.bounds.width() - df::round(media_column.padding.cx * 2 * mc.scale_factor));
		const auto natural_media = media->measure(mc, content_width).cy;
		const auto floor_px = std::min(natural_media, in.bounds.height() / 2);
		media->flex.min_size.cy = std::max(restore_media_min, df::round(floor_px / mc.scale_factor));
	}

	// One rule for both arrangements: the column is centred in the pane, and starts at the top once it
	// is taller than the pane. Verbose metadata used to be held at the top unconditionally, which is
	// right while there is more of it than the pane can hold and reads as a picture pinned to the
	// ceiling over an empty pane when there is not. The offset is continuous in the content height, so
	// growing metadata slides the column to the top rather than snapping there.
	const auto block = calc_flex_layout(*in.priority, mc, in.bounds.extent(), media_column);
	const auto detail_extent = calc_flex_layout(*in.detail, mc, {in.bounds.width(), -1}, media_column).extent;
	const auto block_top = std::max(0, in.bounds.height() - (block.extent.cy + detail_extent.cy)) / 2;

	// Applied here rather than through flex_justify::center, because a centred line reports the
	// container height instead of the content height, so asking where the block ended returned the
	// bottom of the pane and left a gap the size of the free space beneath it.
	const auto origin = pointi{in.bounds.left, in.bounds.top + block_top};

	for (auto i = 0u; i < in.priority->size(); ++i)
	{
		(*in.priority)[i]->layout(mc, block.layout_bounds[i].offset(origin), positions);
	}

	// Detail stacks below the block at its natural height, so it scrolls off the bottom.
	const auto detail_top = block_top + block.extent.cy;
	const recti detail_bounds{
		in.bounds.left, in.bounds.top + detail_top, in.bounds.right, in.bounds.top + detail_top + detail_extent.cy
	};
	layout_flex_elements(*in.detail, mc, positions, detail_bounds, media_column);

	return detail_top + detail_extent.cy;
}

void items_view::layout(ui::measure_context& mc, const sizei extent)
{
	const auto focus = _state.focus_item();
	const auto current_item_anchor = focus && is_visible(focus) ? focus : _layout_center_item;
	auto item_anchor = _pending_items_anchor.valid
		                   ? std::exchange(_pending_items_anchor, {})
		                   : _items_scroller.capture_anchor(current_item_anchor);
	const auto current_media_anchor = find_center_scroll_element(_media_elements, _media_scroller);
	auto media_anchor = _pending_media_anchor.valid
		                    ? std::exchange(_pending_media_anchor, {})
		                    : _media_scroller.capture_anchor(current_media_anchor);

	_client_extent = extent;
	_scroll_width = mc.scroll_width;
	_scale_factor = mc.scale_factor;
	// A zoomed or comparing view gives the whole client to the media element, so neither the list
	// nor the filter box that heads it is laid out or drawn. Focus left in the box would be
	// invisible and, because focus_mode() would still report text_edit, would keep every keyboard
	// command dead until the user found a thumbnail to click.
	if (_display && (_display->is_zoom_mode() || _display->comparing()))
	{
		blur_rendered_filter();
	}
	_top_chrome_height = 0;
	_sidebar_splitter_width = mc.scroll_width;
	const auto sidebar_min = df::round(64 * mc.scale_factor);
	const auto sidebar_max = std::max(sidebar_min, extent.cx / 3);
	const auto sidebar_preferred = setting.sidebar.width > 0
		                               ? df::round(setting.sidebar.width * mc.scale_factor)
		                               : sidebar_host::preferred_width(mc);
	_sidebar_width = std::clamp(sidebar_preferred, sidebar_min, sidebar_max);

	// Regions must exist before anything else in layout asks for the item or media bounds.
	update_regions();

	// Attached whether or not the sidebar is shown. The frame is this view's own, and the sidebar
	// keeps populating, counting and scanning drives while hidden, so binding the handle to
	// visibility would leave all of that running against a frame that does not exist.
	_sidebar->attach_embedded(_host->frame(), _host->owner());

	if (setting.show_sidebar)
	{
		_sidebar->layout_embedded(mc, sidebar_bounds());
	}

	// Keep the edits in sync with the state unless the user is typing in them.
	if (!_filter_edit->focused()) _filter_edit->text(_state.filter().text());
	layout_chrome(mc);

	df::assert_true(ui::is_ui_thread());

	ui::control_layouts positions;

	const auto is_zoom = _display && (_display->is_zoom_mode() || _display->comparing());
	auto avail_media_bounds = calc_media_bounds();

	if (is_zoom)
	{
		if (_media_element)
		{
			_media_element->layout(mc, avail_media_bounds, positions);
			_media_scroller.layout({}, avail_media_bounds, {});
		}
	}
	else
	{
		const media_column_inputs media_inputs{
			&_media_priority_elements, &_media_detail_elements, &_media_elements,
			avail_media_bounds, _media_element
		};
		const auto media_height = layout_media_column(media_inputs, mc, positions);

		const auto split_x = splitter_pos();
		const auto control_padding = mc.scroll_width / 2;
		const recti media_scroll_bounds{
			split_x - control_padding, _client_extent.cy / 2, split_x + control_padding,
			_client_extent.cy
		};

		_media_scroller.layout({avail_media_bounds.width(), media_height}, avail_media_bounds, media_scroll_bounds);
		const auto media_anchor_is_current = media_anchor.element &&
			std::ranges::find(_media_elements, media_anchor.element) != _media_elements.end();
		_media_scroller.restore_anchor(_host, media_anchor, media_anchor_is_current);

		const auto scroll_text_width = mc.measure_text("88888", ui::style::font_face::dialog,
		                                               ui::style::text_style::single_line, _client_extent.cx / 5).cx;
		auto item_layout_iteration_count = 1;

		for (auto i = 0; i < item_layout_iteration_count; i++)
		{
			const auto show_scroll_items = _items_scroller.can_scroll();
			const auto scroll_padding = show_scroll_items ? scroll_text_width : mc.padding1;
			auto avail_item_bounds = calc_items_bounds();
			avail_item_bounds.right -= scroll_padding;

			// locations.md 7.3: the optional rows are budgeted against the height actually
			// available, so a tall window shows everything and a short one degrades gracefully.
			apply_control_row_budget(mc, avail_item_bounds.width(), avail_item_bounds.height() / 4);

			flex_container_layout item_column;
			item_column.direction = flex_direction::column;
			item_column.wrap = flex_wrap::no_wrap;
			item_column.align_items = flex_align::start;
			item_column.padding = {0, df::round(mc.padding2 / mc.scale_factor)};
			const auto items_height = layout_flex_elements(
				_item_elements, mc, positions, avail_item_bounds, item_column).cy;

			const auto item_scroll_bounds = recti{
				_client_extent.cx - scroll_padding, _top_chrome_height, _client_extent.cx, _client_extent.cy
			};
			const sizei scroll_extent = {avail_item_bounds.width(), items_height};
			_items_scroll_top_bounds = _items_scroller.layout_with_footer(
				scroll_extent, avail_item_bounds, item_scroll_bounds,
				show_scroll_items ? scroll_padding : 0, mc.padding1);

			if (show_scroll_items != _items_scroller.can_scroll())
			{
				// can scroll change - redo layout
				item_layout_iteration_count = 2;
			}
		}

		_items_scroll_top->is_visible(_items_scroller.can_scroll());
		if (_items_scroller.can_scroll())
		{
			_items_scroll_top->layout(mc, _items_scroll_top_bounds, positions);
		}

		update_item_scroller_sections();

		const auto item_anchor_element = std::dynamic_pointer_cast<df::item_element>(item_anchor.element);
		const auto item_anchor_is_current = item_anchor_element &&
			_state.display_items().contains(item_anchor_element);
		_items_scroller.restore_anchor(_host, item_anchor, item_anchor_is_current);
	}
	_reset_media_scroll = false;

	// The splitter and scroll bar regions depend on scrollability, which is only known once both
	// scrollers have been laid out.
	update_regions();

	update_visible_items_list();

	_host->frame()->invalidate();
}

void items_view::broadcast_event(const view_element_event& event) const
{
	for (const auto& i : _media_elements)
	{
		i->dispatch_event(event);
	}

	for (const auto& i : _item_elements)
	{
		i->dispatch_event(event);
	}

	_filter_edit->dispatch_event(event);
	_items_scroll_top->dispatch_event(event);

	// The sidebar is a host of its own and app_frame::dpi_changed already walks it, so only the
	// broadcasts nothing else delivers are forwarded here - a second dpi_changed would re-measure
	// every row for nothing.
	if (_sidebar && event.type != view_element_event_type::dpi_changed) _sidebar->broadcast_event(event);
}

class copy_clip_element final : public std::enable_shared_from_this<copy_clip_element>, public view_element
{
	std::function<std::string()> _generate;

public:
	copy_clip_element(std::function<std::string()> generate) : view_element(
		                                                           flex_item::right_justified |
		                                                           view_element_style::has_tooltip |
		                                                           view_element_style::can_invoke),
	                                                           _generate(std::move(generate))
	{
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto bg = calc_background_color(dc);
		xdraw_icon(dc, icon_index::edit_copy, logical_bounds, ui::color(dc.colors.foreground, dc.colors.alpha), bg);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {mc.icon_cxy, mc.icon_cxy};
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke && _generate)
		{
			const auto text = _generate();
			if (!text.empty()) platform::set_clipboard(text);
		}
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		hover.elements->add(make_icon_element(icon_index::edit_copy, flex_item::no_break));
		hover.elements->add(std::make_shared<text_element>(tt.copy_to_clipboard));
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

class url_element final : public std::enable_shared_from_this<url_element>, public view_element
{
	std::vector<std::string> _urls;
	mutable recti _device_bounds;

public:
	url_element(std::vector<std::string> urls) noexcept : view_element(
		                                                      flex_item::right_justified |
		                                                      view_element_style::has_tooltip |
		                                                      view_element_style::can_invoke),
	                                                      _urls(std::move(urls))
	{
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		_device_bounds = logical_bounds;
		const auto bg = calc_background_color(dc);
		xdraw_icon(dc, icon_index::link, logical_bounds, ui::color(dc.colors.foreground, dc.colors.alpha), bg);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {mc.icon_cxy, mc.icon_cxy};
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type != view_element_event_type::invoke || _urls.empty()) return;

		if (_urls.size() == 1)
		{
			platform::open(_urls.front());
			return;
		}

		// Several links means no single obvious destination, so the choice is the user's.
		std::vector<ui::command_ptr> menu;
		menu.reserve(_urls.size());

		for (const auto& url : _urls)
		{
			auto c = std::make_shared<ui::command>();
			c->icon = icon_index::link;
			c->text = url;
			c->invoke = [url] { platform::open(url); };
			menu.emplace_back(std::move(c));
		}

		event.host->track_menu(_device_bounds, menu);
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		hover.elements->add(make_icon_element(icon_index::link, flex_item::no_break));
		hover.elements->add(std::make_shared<text_element>(
			_urls.size() == 1
				? str_format(tt.open_link_fmt.sv(), _urls.front())
				: std::string(tt.open_link_choose)));
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		_device_bounds = bounds.offset(element_offset);
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

inline view_element_ptr media_control_style(view_element_ptr e)
{
	e->padding(4);
	e->margin(4, 0);
	e->set_style_bit(view_element_style::background, true);
	return e;
}

// Media column spacers keep their nominal height; free space centres the column instead.
inline view_element_ptr media_padding(const int height)
{
	auto e = std::make_shared<padding_element>(height);
	e->flex.grow = 0.0f;
	return e;
}

// A section is one panel carrying its own heading, which is what the panel above it already is. Drawn
// as a titled band over unbacked content, the heading read as a control and the sections needed a rule
// between them to be told apart; as one panel each, the gap between panels is what separates them and
// the rule is noise. Nothing inside grows: the column measures the section at its natural height, and
// a child claiming free space would open a gap between the heading and its own content.
static view_element_ptr make_section(view_element_ptr title, view_element_ptr content)
{
	auto section = std::make_shared<view_elements>(flex_item::stretch);
	section->flex_container.direction = flex_direction::column;
	section->flex_container.wrap = flex_wrap::no_wrap;
	section->flex_container.align_items = flex_align::stretch;
	section->flex_container.gap = {0, 4};
	section->add(std::move(title));
	if (content) section->add(std::move(content));
	section->padding = {8, 8};
	section->margin = {4, 8};
	section->set_style_bit(view_element_style::background, true);
	return section;
}

static std::string_view format_metadata_standard(const metadata_standard ms)
{
	switch (ms)
	{
	case metadata_standard::media: return tt.media_metadata_title;
	case metadata_standard::exif: return tt.exif_metadata_title;
	case metadata_standard::iptc: return tt.iptc_metadata_title;
	case metadata_standard::xmp: return tt.xmp_metadata_title;
	case metadata_standard::raw: return tt.raw_metadata_title;
	case metadata_standard::ffmpeg: return tt.media_metadata_title;
	case metadata_standard::icc: return tt.icc_metadata_title;
	// Structural, not a published metadata standard, so it carries a plain descriptive heading.
	case metadata_standard::structure: return "File structure";
	}

	return {};
}

class cover_art_control final : public view_element, public std::enable_shared_from_this<cover_art_control>
{
public:
	df::item_element_ptr _item;
	mutable ui::const_surface_ptr _tex_source;
	mutable ui::texture_ptr _tex;
	mutable int _cx_surface = 0;

	std::shared_ptr<ui::group_control> _controls = std::make_shared<ui::group_control>();

	cover_art_control(df::item_element_ptr item) :
		view_element(view_element_style::has_tooltip | view_element_style::can_invoke), _item(std::move(item))
	{
	}

	void add(const view_element_ptr& p) const
	{
		_controls->add(p);
	}

	// Read from the item rather than held: the surface is decoded by the render worker and dropped by
	// resource cleanup, so a copy kept here would outlive the eviction and miss the restage.
	ui::const_surface_ptr surface() const
	{
		return _item ? _item->cover_art_surface() : nullptr;
	}

	bool is_control_area(const pointi loc, const pointi element_offset) const override
	{
		return _controls->is_control_area(loc, element_offset);
	}

	void dispatch_event(const view_element_event& event) override
	{
		// The cached texture belongs to the device being torn down, and nothing else releases it.
		if (event.type == view_element_event_type::free_graphics_resources)
		{
			_tex.reset();
			_tex_source.reset();
		}

		_controls->dispatch_event(event);
	}

	void hover(interaction_context& ic) override
	{
		_controls->hover(ic);
	}

	sizei measure(ui::measure_context& mc, const int cx) const override
	{
		auto avail = cx;
		auto cy = 0;

		_cx_surface = 0;

		if (const auto s = surface(); is_valid(s))
		{
			const auto surf_cx = s->width() + mc.padding2;
			const auto show_surface = static_cast<int>(surf_cx) < cx / 2;

			if (show_surface)
			{
				_cx_surface = surf_cx;
				cy = s->height() + mc.padding2 * 2;
				avail -= surf_cx;
			}
		}

		const auto controls_extent = _controls->measure(mc, avail);
		cy = std::max(cy, controls_extent.cy);
		return {cx, cy};
	}

	void layout(ui::measure_context& mc, const recti bounds, ui::control_layouts& positions) override
	{
		view_element::layout(mc, bounds, positions);

		auto control_bounds = bounds;
		control_bounds.left += _cx_surface;
		_controls->layout(mc, control_bounds, positions);
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto s = surface();

		if (_cx_surface > 0 && is_valid(s))
		{
			// Keyed on the surface, because a restage after eviction produces a different one and a
			// texture built from the old surface would be the picture that was evicted.
			if (!_tex || _tex_source != s)
			{
				const auto t = dc.create_texture();

				if (t && t->update(s) != ui::texture_update_result::failed)
				{
					_tex = t;
					_tex_source = s;
				}
			}

			if (_tex)
			{
				const auto extent = _tex->dimensions();
				auto top_left = bounds.top_left();
				top_left.x += dc.padding2 / 2;
				top_left.y += dc.padding2;

				const recti surface_bounds = {top_left, extent};
				dc.draw_texture(_tex, surface_bounds.offset(element_offset));
			}
		}

		_controls->render(dc, element_offset);
	}

	void visit_controls(const std::function<void(const ui::control_base_ptr&)>& handler) override
	{
		_controls->visit_controls(handler);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return _controls->controller_from_location(host, loc, element_offset, ctx);
	}
};

// A container is opened by default only while it stays small enough to read at a glance; beyond
// that the user asks for it.
static constexpr auto metadata_auto_expand_limit = 32;

static std::string metadata_row_key(const std::string_view prefix, const metadata_kv& row)
{
	const auto id = row.id.empty() ? std::string_view(row.key) : std::string_view(row.id);
	return std::format("{}/{}", prefix, id);
}

class numeric_table_control final : public view_element
{
	std::vector<uint16_t> _values;
	int _columns;
	mutable int _cell_width = 0;
	mutable int _line_height = 0;
	mutable int _table_width = 0;

public:
	numeric_table_control(std::vector<uint16_t> values, const int columns) :
		_values(std::move(values)), _columns(std::max(1, columns))
	{
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto left = logical_bounds.left + std::max(0, (logical_bounds.width() - _table_width) / 2);
		const auto color = ui::color(dc.colors.foreground, dc.colors.alpha * 0.77f);

		for (auto i = 0_z; i < _values.size(); ++i)
		{
			std::array<char, 5> text{};
			const auto [end, ec] = std::to_chars(text.data(), text.data() + text.size(), _values[i]);
			if (ec != std::errc{}) continue;

			const auto column = static_cast<int>(i % _columns);
			const auto row = static_cast<int>(i / _columns);
			const auto cell = recti(left + column * _cell_width, logical_bounds.top + row * _line_height,
			                        left + (column + 1) * _cell_width, logical_bounds.top + (row + 1) * _line_height);
			dc.draw_text(std::string_view(text.data(), end - text.data()), cell, ui::style::font_face::code,
			             ui::style::text_style::single_line_far, color, {});
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto extent = mc.measure_text("65535"sv, ui::style::font_face::code,
		                                    ui::style::text_style::single_line, width_limit);
		_line_height = extent.cy + df::round(2.0 * mc.scale_factor);
		_cell_width = std::min(extent.cx + df::round(8.0 * mc.scale_factor), width_limit / _columns);
		_table_width = _cell_width * _columns;
		const auto rows = df::round_up(static_cast<int>(_values.size()), _columns);
		return {std::min(width_limit, _table_width), rows * _line_height};
	}
};

class metadata_tree_control;

// Nesting is carried by connector lines drawn from each parent down through its children, so the
// listing owns its own rows, measurement, hit testing and expansion rather than composing cells.
class metadata_tree_control final : public std::enable_shared_from_this<metadata_tree_control>, public view_element
{
	static constexpr int col_padding = 8;

	struct row
	{
		std::string key;
		std::string value;
		std::string shape;
		std::string detail;
		std::string id;
		int depth = 0;
		int child_count = 0;
		bool container = false;
		bool expandable = false;
		bool last_sibling = true;
		bool default_open = false;
		bool is_raw = false; // detail holds the original packet text rather than a hex dump
		bool prose = false;
		std::shared_ptr<view_element> detail_control;
	};

	struct visible_row
	{
		int index = 0;
		int top = 0;
		int height = 0;
		int detail_height = 0;
		int detail_width = 0;
		uint32_t spines = 0; // bit c set while an ancestor's connector column c passes through
		bool open = false;
	};

	// One bit of spines a connector column. A path deeper than this draws no spine beyond the last
	// column rather than shifting past the width of the mask, which is undefined and on MSVC wraps
	// the count to draw a spine belonging to column zero.
	static constexpr int max_spine_columns = 32;

	std::vector<row> _rows;
	metadata_tree_state_ptr _tree_state;

	mutable std::vector<visible_row> _visible;
	mutable int _line_height = 0;
	mutable int _indent = 0;
	mutable int _key_width = 0;
	mutable int _value_width = 0;
	mutable int _shape_width = 0;
	mutable int _detail_left = 0;
	mutable int _total_height = 0;
	int _hot = -1;

public:
	metadata_tree_control(const metadata_block& block, metadata_tree_state_ptr tree_state,
	                      const view_element_options& options) : view_element(options),
	                                                             _tree_state(std::move(tree_state))
	{
		build(block.values, std::format("{}", static_cast<int>(block.standard)), block.raw);
	}

	metadata_tree_control(const metadata_kv_list& values, const std::string_view id_prefix,
	                      metadata_tree_state_ptr tree_state, const view_element_options& options) :
		view_element(options), _tree_state(std::move(tree_state))
	{
		build(values, id_prefix, {});
	}

	// A detail control can hold a texture - the embedded thumbnail is a surface_element - and the
	// rows are this control's own, so the broadcast has to be carried down to them by hand. Only the
	// broadcast: click, invoke and the rest are this control's own to interpret, and its controller
	// already does, so forwarding them would give one gesture two readers.
	void dispatch_event(const view_element_event& event) override
	{
		if (event.type != view_element_event_type::free_graphics_resources &&
			event.type != view_element_event_type::dpi_changed)
			return;

		for (const auto& r : _rows)
		{
			if (r.detail_control) r.detail_control->dispatch_event(event);
		}
	}

private:
	void build(const metadata_kv_list& values, const std::string_view id_prefix, const std::string_view raw)
	{
		_rows.reserve(values.size() + 1);

		for (const auto& r : values)
		{
			auto& added = _rows.emplace_back();
			added.key = r.key;
			added.value = r.value;
			added.shape = r.shape;
			if (const auto binary = std::get_if<metadata_binary_detail>(&r.detail))
			{
				added.detail_control = std::make_shared<hex_control>(binary->bytes, flex_item::center);
			}
			else if (const auto image = std::get_if<metadata_image_detail>(&r.detail); image && image->surface)
			{
				// surface_element releases its texture when the graphics device goes away.
				added.detail_control = std::make_shared<surface_element>(image->surface, 0, flex_item::center);
			}
			else if (const auto numeric = std::get_if<metadata_numeric_detail>(&r.detail);
				numeric && !numeric->values.empty() && numeric->columns > 0)
			{
				added.detail_control = std::make_shared<numeric_table_control>(numeric->values, numeric->columns);
			}
			else if (const auto text = std::get_if<metadata_text_detail>(&r.detail))
			{
				added.detail = text->text;
			}
			added.id = metadata_row_key(id_prefix, r);
			added.depth = r.depth;
			added.container = r.container;
			added.default_open = r.open_by_default;
			added.prose = r.prose;
		}

		if (!raw.empty())
		{
			auto& added = _rows.emplace_back();
			added.key = "Raw block";
			added.value = std::format("{} bytes", raw.size());
			added.detail = raw;
			added.id = std::format("{}/raw", id_prefix);
			added.is_raw = true;
		}

		// A container owns every following row until the depth returns to its own level.
		for (auto i = 0_z; i < _rows.size(); ++i)
		{
			if (_rows[i].container)
			{
				auto j = i + 1;
				while (j < _rows.size() && _rows[j].depth > _rows[i].depth) ++j;
				_rows[i].child_count = static_cast<int>(j - i - 1);
			}

			_rows[i].expandable = _rows[i].container || !_rows[i].detail.empty() || _rows[i].detail_control;
		}

		std::vector<bool> seen;

		for (auto i = _rows.size(); i-- > 0;)
		{
			const auto depth = static_cast<size_t>(_rows[i].depth);
			if (seen.size() <= depth) seen.resize(depth + 1, false);
			_rows[i].last_sibling = !seen[depth];
			seen[depth] = true;
			for (auto a = depth + 1; a < seen.size(); ++a) seen[a] = false;
		}
	}

public:

	// Reproduces the listing as text: every row indented as drawn, hex dumps left out, and the
	// original packet appended when the standard carries one.
	std::string copy_text(const std::string_view title) const
	{
		std::string result(title);
		result += '\n';

		for (const auto& r : _rows)
		{
			result.append(static_cast<size_t>(std::max(0, r.depth)) * 2, ' ');
			result += r.key;

			// Prose carries its whole text in the detail, so the one-line preview would truncate it.
			if (r.prose)
			{
				result += ":\n";
				result += r.detail;
				result += '\n';
				continue;
			}

			if (!r.value.empty())
			{
				result += ": ";
				result += r.value;
			}

			if (!r.shape.empty())
			{
				result += "  [";
				result += r.shape;
				result += ']';
			}

			result += '\n';

			if (r.is_raw)
			{
				result += '\n';
				result += r.detail;
				result += '\n';
			}
		}

		return result;
	}

	bool is_open(const row& r) const
	{
		const auto found = _tree_state->expanded.find(r.id);
		if (found != _tree_state->expanded.end()) return found->second;
		if (r.default_open) return true;
		return r.container && r.child_count <= metadata_auto_expand_limit;
	}

	int row_from_location(const pointi loc, const pointi element_offset) const
	{
		const auto y = loc.y - element_offset.y - bounds.top;

		for (auto i = 0_z; i < _visible.size(); ++i)
		{
			if (y >= _visible[i].top && y < _visible[i].top + _line_height)
			{
				return _rows[_visible[i].index].expandable ? static_cast<int>(i) : -1;
			}
		}

		return -1;
	}

	int hot() const
	{
		return _hot;
	}

	void set_hot(const int visible_index)
	{
		_hot = visible_index;
	}

	void toggle(const int visible_index)
	{
		if (visible_index < 0 || visible_index >= static_cast<int>(_visible.size())) return;

		const auto& r = _rows[_visible[visible_index].index];
		if (!r.expandable) return;

		_tree_state->expanded[r.id] = !is_open(r);
		_hot = -1;
		if (_tree_state->invalidate) _tree_state->invalidate();
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);
		const auto clr_dim = ui::color(dc.colors.foreground, dc.colors.alpha * 0.55f);
		const auto clr_line = ui::color(dc.colors.foreground, dc.colors.alpha * 0.3f);
		const auto thickness = std::max(1, df::round(dc.scale_factor));

		for (auto i = 0_z; i < _visible.size(); ++i)
		{
			const auto& v = _visible[i];
			const auto top = logical_bounds.top + v.top;
			const auto& r = _rows[v.index];
			const auto mid = top + _line_height / 2;

			for (auto c = 0; c + 1 < r.depth && c < max_spine_columns; ++c)
			{
				if (v.spines & (1u << c))
				{
					const auto x = logical_bounds.left + c * _indent + _indent / 2;
					dc.draw_rect(recti(x, top, x + thickness, top + v.height), clr_line);
				}
			}

			if (r.depth > 0)
			{
				const auto x = logical_bounds.left + (r.depth - 1) * _indent + _indent / 2;
				const auto elbow_bottom = r.last_sibling ? mid : top + v.height;
				dc.draw_rect(recti(x, top, x + thickness, elbow_bottom), clr_line);
				dc.draw_rect(recti(x, mid, logical_bounds.left + r.depth * _indent, mid + thickness), clr_line);
			}

			// The children of an open container hang from a spine that starts under its own row.
			if (v.open && r.container && r.child_count > 0)
			{
				const auto x = logical_bounds.left + r.depth * _indent + _indent / 2;
				dc.draw_rect(recti(x, mid, x + thickness, top + v.height), clr_line);
			}

			if (static_cast<int>(i) == _hot)
			{
				dc.draw_rounded_rect(recti(logical_bounds.left, top, logical_bounds.right, top + _line_height),
				                     ui::color(ui::style::color::view_selected_background, dc.colors.alpha * 0.25f),
				                     dc.padding1);
			}

			auto x = logical_bounds.left + r.depth * _indent;
			const auto key_right = logical_bounds.left + _key_width - col_padding;

			if (x < key_right)
			{
				dc.draw_text(r.key, recti(x, top, key_right, top + _line_height), ui::style::font_face::dialog,
				             ui::style::text_style::single_line, r.container ? clr : clr_dim, {});
			}

			x = logical_bounds.left + _key_width;
			// An open prose row shows its full text below, so the preview line is not repeated.
			if (!(r.prose && v.open))
			{
				dc.draw_text(r.value, recti(x, top, x + _value_width - col_padding, top + _line_height),
				             ui::style::font_face::dialog, ui::style::text_style::single_line, clr, {});
			}

			if (_shape_width > 0)
			{
				x = logical_bounds.left + _key_width + _value_width;
				dc.draw_text(r.shape, recti(x, top, x + _shape_width - col_padding, top + _line_height),
				             ui::style::font_face::dialog, ui::style::text_style::single_line_far, clr_dim, {});
			}

			if (v.detail_height > 0)
			{
				const auto detail_top = top + _line_height;
				if (r.detail_control)
				{
					// The child is drawn in the box it measured, centred. Handing it the whole row
					// instead stretches any element that fills its bounds, such as an image.
					const auto avail_width = std::max(0, logical_bounds.width() - _detail_left);
					const auto cx = v.detail_width > 0 ? std::min(v.detail_width, avail_width) : avail_width;
					const auto left = _detail_left + (avail_width - cx) / 2;

					r.detail_control->bounds = recti(left, v.top + _line_height, left + cx, v.top + v.height);
					r.detail_control->render(dc, {logical_bounds.left, logical_bounds.top});
				}
				else
				{
					dc.draw_text(r.detail,
					             recti(logical_bounds.left + _detail_left, detail_top, logical_bounds.right,
					                   detail_top + v.detail_height),
					             r.prose ? ui::style::font_face::dialog : ui::style::font_face::code,
					             ui::style::text_style::multiline, r.prose ? clr : clr_dim, {});
				}
			}
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto text_height = mc.text_line_height(ui::style::font_face::dialog);
		_line_height = text_height + df::round(4.0 * mc.scale_factor);
		_indent = text_height;
		_visible.clear();
		_visible.reserve(_rows.size());

		std::vector<bool> spines;
		auto hidden_below = -1;
		auto key_natural = 0;
		auto value_natural = 0;
		auto shape_natural = 0;

		for (auto i = 0_z; i < _rows.size(); ++i)
		{
			const auto& r = _rows[i];

			if (hidden_below >= 0)
			{
				if (r.depth > hidden_below) continue;
				hidden_below = -1;
			}

			const auto open = r.expandable && is_open(r);
			if (r.container && !open) hidden_below = r.depth;

			const auto depth = static_cast<size_t>(r.depth);
			if (spines.size() < depth) spines.resize(depth, false);

			uint32_t mask = 0;
			for (auto c = 0_z; c + 1 < depth && c < static_cast<size_t>(max_spine_columns); ++c)
			{
				if (spines[c]) mask |= 1u << c;
			}

			if (depth > 0)
			{
				spines[depth - 1] = !r.last_sibling;
			}

			auto& v = _visible.emplace_back();
			v.index = static_cast<int>(i);
			v.spines = mask;
			v.open = open;

			const auto key_size = mc.measure_text(r.key, ui::style::font_face::dialog,
			                                      ui::style::text_style::single_line, width_limit, _line_height);
			key_natural = std::max(key_natural, r.depth * _indent + key_size.cx + col_padding);

			const auto value_size = mc.measure_text(r.value, ui::style::font_face::dialog,
			                                        ui::style::text_style::single_line, width_limit, _line_height);
			value_natural = std::max(value_natural, value_size.cx + col_padding);

			if (!r.shape.empty())
			{
				const auto shape_size = mc.measure_text(r.shape, ui::style::font_face::dialog,
				                                        ui::style::text_style::single_line, width_limit,
				                                        _line_height);
				shape_natural = std::max(shape_natural, shape_size.cx + col_padding);
			}
		}

		// A pane narrow enough to leave no room at all still has to yield ordered, non-inverted
		// column bounds: std::clamp is undefined when its high bound falls below its low bound.
		const auto available = std::max(0, width_limit);
		_shape_width = std::min(shape_natural, available / 4);
		_key_width = std::clamp(key_natural, 0, std::max(0, (available - _shape_width) / 2));
		_value_width = std::max(0, available - _key_width - _shape_width);
		_detail_left = std::min(_key_width, _indent * 2);

		_total_height = 0;

		for (auto& v : _visible)
		{
			const auto& r = _rows[v.index];
			v.top = _total_height;
			v.detail_height = 0;

			if (v.open && (!r.detail.empty() || r.detail_control))
			{
				const auto avail_width = std::max(_indent, width_limit - _detail_left);

				if (r.detail_control)
				{
					const auto extent = r.detail_control->measure(mc, avail_width);
					v.detail_height = extent.cy;
					v.detail_width = extent.cx;
				}
				else
				{
					v.detail_height = mc.measure_text(r.detail,
					                                  r.prose
						                                  ? ui::style::font_face::dialog
						                                  : ui::style::font_face::code,
					                                  ui::style::text_style::multiline, avail_width).cy;
				}
			}

			v.height = _line_height + v.detail_height;
			_total_height += v.height;
		}

		return {width_limit, _total_height};
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;
};

// Hovering and clicking are resolved per row, so the whole listing latches one controller and
// resolves the row from the pointer each time rather than splitting into per-row elements.
class metadata_tree_controller final : public view_controller
{
	std::shared_ptr<metadata_tree_control> _control;
	pointi _element_offset;
	// The row the press landed on. A release resolves its own row and the two have to agree, or a
	// press that drifted would act on whichever row it drifted onto.
	int _pressed_row = -1;

public:
	metadata_tree_controller(const view_host_ptr& host, std::shared_ptr<metadata_tree_control> control,
	                         const pointi element_offset, const recti bounds) : view_controller(host, bounds),
		_control(std::move(control)), _element_offset(element_offset)
	{
	}

	~metadata_tree_controller() override
	{
		if (_control->hot() != -1)
		{
			_control->set_hot(-1);
			_host->frame()->invalidate(_bounds);
		}
	}

	void update_hot(const pointi loc)
	{
		_last_loc = loc;
		const auto row = _bounds.contains(loc) ? _control->row_from_location(loc, _element_offset) : -1;

		if (row != _control->hot())
		{
			_control->set_hot(row);
			_host->frame()->invalidate(_bounds);
		}
	}

	void on_mouse_move(const pointi loc) override
	{
		update_hot(loc);
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_last_loc = loc;
		_pressed_row = _bounds.contains(loc) ? _control->row_from_location(loc, _element_offset) : -1;
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		// The row that was pressed is the row that toggles. Reading the release alone meant a press
		// that drifted onto a neighbour, or off the control entirely, expanded or collapsed a row
		// the user had never pointed at.
		const auto row = _bounds.contains(loc) ? _control->row_from_location(loc, _element_offset) : -1;

		if (row >= 0 && row == _pressed_row) _control->toggle(row);

		_pressed_row = -1;
	}

	ui::style::cursor cursor() const override
	{
		return _control->hot() >= 0 ? ui::style::cursor::link : ui::style::cursor::normal;
	}
};

inline view_controller_ptr metadata_tree_control::controller_from_location(
	const view_host_ptr& host, const pointi loc, const pointi element_offset,
	hit_test_context& ctx)
{
	if (!is_visible() || !bounds.contains(loc - element_offset)) return nullptr;

	auto result = std::make_shared<metadata_tree_controller>(host, shared_from_this(), element_offset,
	                                                         bounds.offset(element_offset));
	result->update_hot(loc);
	return result;
}

void items_view::add_metadata_elements(std::vector<view_element_ptr>& elements, const metadata_block& block)
{
	df::assert_true(ui::is_ui_thread());

	if (block.values.empty() && block.raw.empty() && block.parsed) return;

	auto value_count = 0;

	for (const auto& r : block.values)
	{
		if (!r.container) ++value_count;
	}

	// The block reports its own extent and whether it was understood, so an unreadable block is
	// not mistaken for an absent one.
	auto title = std::string(format_metadata_standard(block.standard));
	std::string summary;

	if (block.bytes > 0) summary = df::format_size(df::file_size(block.bytes));

	if (value_count > 0)
	{
		if (!summary.empty()) summary += ", ";
		summary += std::format("{} values", value_count);
	}

	if (!block.parsed)
	{
		if (!summary.empty()) summary += ", ";
		summary += "not understood";
	}

	if (!summary.empty()) title = std::format("{}  ({})", title, summary);

	auto tree = std::make_shared<metadata_tree_control>(block, _metadata_tree, flex_item::stretch);

	// The copy button reproduces the tree, so it holds the tree alive itself. Built in its own
	// statement because the section below moves `tree`: function arguments are indeterminately
	// sequenced, MSVC evaluates them right to left, and capturing in one argument while moving in
	// another left the lambda holding an emptied pointer. The listing still drew, because the
	// section owns the tree either way - only pressing copy dereferenced null.
	auto section_title = std::make_shared<group_title_control>(title, std::vector<view_element_ptr>{
		std::make_shared<copy_clip_element>([tree, title] { return tree->copy_text(title); })
	});

	elements.emplace_back(make_section(std::move(section_title), std::move(tree)));
}

// Every prose field the item carries reads as one section. A lone field is just its text; a list
// becomes a tree so the secondary fields cost one line each until the user opens them.

// A collapsed row is one line, so its preview cannot keep the line breaks and runs of space that
// would leave it looking blank or ragged.
static std::string single_line_preview(const std::string_view text)
{
	std::string result;
	result.reserve(text.size());
	auto pending_space = false;

	for (const auto c : text)
	{
		if (static_cast<uint8_t>(c) <= ' ')
		{
			pending_space = !result.empty();
			continue;
		}

		if (pending_space)
		{
			result += ' ';
			pending_space = false;
		}

		result += c;
	}

	return result;
}

void items_view::add_description_elements(std::vector<view_element_ptr>& elements, const df::item_element_ptr& item,
                                          const prop::item_metadata_const_ptr& md)
{
	df::assert_true(ui::is_ui_thread());

	if (!md) return;

	const auto fields = prop::descriptive_fields(*md);
	if (fields.empty()) return;

	// The header names the content: the field itself while there is only one, the section once
	// there is a list to hold together.
	const auto title = fields.size() == 1 ? fields.front().name : std::string_view(tt.prop_name_description);

	std::string all_text;
	std::string copy_text;

	for (const auto& f : fields)
	{
		if (!all_text.empty()) all_text += '\n';
		all_text += f.text.sv();

		if (fields.size() > 1)
		{
			copy_text += f.name;
			copy_text += ":\n";
		}

		copy_text += f.text.sv();
		copy_text += '\n';
	}

	std::vector<view_element_ptr> buttons;

	if (auto urls = df::url_extract_all(all_text); !urls.empty())
	{
		buttons.emplace_back(std::make_shared<url_element>(std::move(urls)));
	}

	buttons.emplace_back(std::make_shared<copy_clip_element>([text = std::move(copy_text)] { return text; }));

	if (auto edit_command = _state.find_command(commands::tool_edit_description))
	{
		buttons.emplace_back(std::make_shared<command_link_element>(std::move(edit_command)));
	}

	view_element_ptr body;

	// selection-controls.md: one field is its text alone, several become the tree. There was a byte
	// bound here that collapsed a long lone field to one clipped line with nothing to say it could be
	// opened -- and the eviction it was written for is gone, because the stream summary it used to
	// displace now sits above this section rather than below it. A byte count was also the wrong
	// measure: 400 bytes is a paragraph in English and a sentence in Japanese.
	if (fields.size() == 1)
	{
		body = std::make_shared<text_element>(fields.front().text);
	}
	else
	{
		metadata_kv_list rows;
		rows.reserve(fields.size());

		for (const auto& f : fields)
		{
			// A repeat is still worth listing -- both fields really are populated -- but not worth
			// reading twice, so it says so and opens only if asked.
			auto& row = rows.emplace_back(f.name, f.duplicate
				                                      ? std::string(tt.duplicate_text)
				                                      : single_line_preview(f.text.sv()));
			row.detail = metadata_text_detail{std::string(f.text.sv())};
			row.id = f.id;
			row.prose = true;
			row.open_by_default = rows.size() == 1;
		}

		body = std::make_shared<metadata_tree_control>(rows, "description"sv, _metadata_tree, flex_item::stretch);
	}

	view_element_ptr content = std::move(body);

	if (item->has_cover_art())
	{
		auto cover = std::make_shared<cover_art_control>(item);
		cover->add(content);
		content = std::move(cover);
	}

	elements.emplace_back(make_section(std::make_shared<group_title_control>(title, buttons), std::move(content)));
}

// What is inside the container, which is the question a media file raises before any other. It sits
// in the primary block rather than behind the verbose toggle, because an answer below the fold is
// not an answer.
void items_view::add_stream_elements(std::vector<view_element_ptr>& elements, const display_state_ptr& display,
                                     const df::item_element_ptr& item)
{
	df::assert_true(ui::is_ui_thread());

	const auto table = std::make_shared<ui::table_element>(flex_item::stretch);
	table->no_shrink_col[0] = true;
	table->no_shrink_col[1] = true;
	table->no_shrink_col[2] = false;
	table->no_shrink_col[3] = true;
	table->no_shrink_col[4] = true;
	table->no_shrink_col[5] = true;
	table->no_shrink_col[6] = true;

	auto audio_track_number = 0;

	for (const auto& st : display->_player_media_info.streams)
	{
		std::string type;

		switch (st.type)
		{
		case av_stream_type::video: type = tt.video;
			break;
		case av_stream_type::audio: type = tt.audio;
			break;
		case av_stream_type::data: type = tt.data;
			break;
		case av_stream_type::subtitle: type = tt.subtitle;
			break;
		}

		auto format = st.pixel_format;
		auto codec = st.codec;

		if (st.type == av_stream_type::video)
		{
			// Tokens in the same terse form as the audio row's, which is the form the format and
			// codec names already take.
			const auto append = [](std::string& text, const std::string_view token)
			{
				if (token.empty()) return;
				if (!text.empty()) text += "  ";
				text += token;
			};

			append(codec, st.profile);
			if (st.bit_depth > 0) append(format, std::format("{}bit", st.bit_depth));
			if (!st.hdr_transfer.empty()) append(format, std::format("HDR {}", st.hdr_transfer));
		}

		if (st.type == av_stream_type::audio)
		{
			++audio_track_number;
			format = prop::format_audio_sample_rate(st.audio_sample_rate);

			if (st.audio_channels != 0)
			{
				format += "  ";
				format += prop::format_audio_channels(st.audio_channels);
			}

			if (st.audio_sample_type != prop::audio_sample_t::none)
			{
				format += "  ";
				format += format_audio_sample_type(st.audio_sample_type);
			}
		}

		auto stream = std::make_shared<stream_element>(_state, item, st, audio_track_number);

		std::vector<view_element_ptr> row = {
			std::make_shared<text_element>(str::to_string(st.index)),
			std::make_shared<text_element>(type),
			stream,
			std::make_shared<text_element>(codec),
			std::make_shared<text_element>(st.fourcc),
			std::make_shared<text_element>(format),
			std::make_shared<text_element>(st.rotation == 0.0
				                               ? std::string{}
				                               : std::format("rotation={}", st.rotation))
		};

		table->add(row);
	}

	elements.emplace_back(make_section(std::make_shared<group_title_control>(tt.prop_name_streams), table));
}

void items_view::update_media_elements()
{
	df::assert_true(ui::is_ui_thread());
	if (!_reset_media_scroll)
	{
		_pending_media_anchor = _media_scroller.capture_anchor(
			find_center_scroll_element(_media_elements, _media_scroller));
	}

	const auto display = _state.display_state();
	_display = display;

	// Only some selections produce a media element. Without this the previous item's control -- and
	// through it its display state, decoded surface and player references -- would be retained for
	// the whole of the next selection and could still answer the zoom and comparison paths.
	_media_element.reset();

	std::vector<view_element_ptr> elements;
	view_element_ptr media_control_element;
	view_element_ptr priority_end_element;

	if (display)
	{
		media_control_element = _state.create_selection_controls();
		priority_end_element = media_control_element;

		if (display->is_one())
		{
			const auto& item = display->_item1;
			const auto* const file_type = item->file_type();
			const auto md = item->metadata();

			// A media file the decoder could not open has nothing to play or draw, so it falls through
			// to the hex dump rather than presenting an empty player.
			const auto media_unavailable = display->_av_open_failed;

			if (file_type->has_trait(file_traits::bitmap))
			{
				_media_element = std::make_shared<photo_control>(_state, display, _host);

				elements.emplace_back(_media_element);
				elements.emplace_back(media_padding(4));
				elements.emplace_back(media_control_style(media_control_element));
			}
			else if (file_type->has_trait(file_traits::visualize_audio) && !media_unavailable)
			{
				_media_element = std::make_shared<audio_control>(_state, display, _host);

				elements.emplace_back(_media_element);
				elements.emplace_back(media_padding(4));
				elements.emplace_back(media_control_style(media_control_element));
			}
			else if (file_type->has_trait(file_traits::av) && !media_unavailable)
			{
				_media_element = std::make_shared<video_control>(_state, display, _host);

				elements.emplace_back(_media_element);
				elements.emplace_back(media_padding(4));
				elements.emplace_back(media_control_style(media_control_element));
			}
			else if (file_type->has_trait(file_traits::archive))
			{
				// These panels are a stack of listings, not a media pane with details under it, so no
				// part of them is held on screen while the rest scrolls away.
				priority_end_element.reset();
				elements.emplace_back(media_control_style(media_control_element));
				elements.emplace_back(media_padding(4));
				elements.emplace_back(std::make_shared<file_list_control>(display, flex_item::center));
			}
			else if (file_type->has_trait(file_traits::commodore))
			{
				priority_end_element.reset();
				elements.emplace_back(media_control_style(media_control_element));

				if (display && !display->_selected_item_data.empty())
				{
					elements.emplace_back(media_padding(4));
					elements.emplace_back(std::make_shared<commodore_disk_control>(display, flex_item::center));

					// Single-file program containers also show a hex view of the content.
					if (file_type->extension == "prg" || file_type->extension == "p00")
					{
						elements.emplace_back(media_padding(4));
						elements.emplace_back(std::make_shared<hex_control>(display, flex_item::center));
					}
				}
			}
			else
			{
				priority_end_element.reset();
				elements.emplace_back(media_control_style(media_control_element));

				if (display && !display->_selected_item_data.empty())
				{
					elements.emplace_back(std::make_shared<hex_control>(display, flex_item::center));

					if (item->file_size().to_int64() > df::one_meg)
					{
						elements.emplace_back(std::make_shared<divider_element>());
						auto element = std::make_shared<text_element>(tt.truncated_at_one_mb,
						                                              ui::style::font_face::title,
						                                              ui::style::text_style::multiline_center,
						                                              flex_item::center |
						                                              view_element_style::important);
						element->padding = {8, 8};
						element->margin = {8, 8};
						elements.emplace_back(element);
					}
					else
					{
						elements.emplace_back(media_padding(32));
					}
				}
			}

			// The toggle only earns its place when there is something behind it to reveal. The stream
			// summary is no longer behind it: what is inside the container is the first question a
			// media file raises, and answering it below the fold is not answering it.
			const auto& media_info = display->_player_media_info;
			const auto has_streams = !media_info.streams.empty();
			// The same test add_metadata_elements admits a block by. A packet that could not be parsed
			// still draws as a block reporting that, so counting values alone withheld the toggle from
			// the one file whose metadata the reader most needs to see.
			const auto has_verbose_content = std::ranges::any_of(media_info.metadata,
			                                                     [](const auto& m)
			                                                     {
				                                                     return !m.values.empty() || !m.raw.empty() ||
					                                                     !m.parsed;
			                                                     });

			if (has_streams)
			{
				add_stream_elements(elements, display, item);
				priority_end_element = elements.back();
			}

			const auto has_long_form = md && (!is_empty(md->comment) || !is_empty(md->description) ||
				!is_empty(md->synopsis));

			// A lone affordance must not push the centred media into a scrolling pane, so with verbose
			// metadata closed and nothing else below the priority block the toggle joins that block.
			// Once verbose metadata is open the pane scrolls anyway and the toggle is low-value chrome,
			// so it returns to the base of the stack rather than spending priority space. Otherwise it
			// trails the whole optional-detail run, so Comment and Description are read as detail.
			const auto toggle_in_priority = has_verbose_content && !setting.verbose_metadata && !has_long_form &&
				!elements.empty() && elements.back() == priority_end_element;

			auto append_verbose_toggle = [&elements]
			{
				// No rule above it: each section is its own panel now, so the gap between panels is what
				// separates them and a line as well would be saying it twice.
				auto verbose_element = std::make_shared<link_element>(
					setting.verbose_metadata ? tt.hide_verbose_metadata : tt.show_verbose_metadata,
					commands::verbose_metadata, ui::style::font_face::dialog,
					ui::style::text_style::multiline_center, flex_item::center);
				elements.emplace_back(verbose_element);
				elements.emplace_back(media_padding(8));
			};

			if (toggle_in_priority)
			{
				append_verbose_toggle();
				priority_end_element = elements.back();
			}

			add_description_elements(elements, item, md);

			if (setting.verbose_metadata)
			{
				if (display && !display->_player_media_info.metadata.empty())
				{
					for (const auto& block : display->_player_media_info.metadata)
					{
						add_metadata_elements(elements, block);
					}
				}
			}

			if (has_verbose_content && !toggle_in_priority)
			{
				append_verbose_toggle();
			}
		}
		else if (display->is_two())
		{
			_media_element = std::make_shared<side_by_side_control>(_state, display, _host);
			elements.emplace_back(_media_element);
			elements.emplace_back(media_control_style(media_control_element));
		}
		else if (display->is_multi())
		{
			_media_element = std::make_shared<images_control2>(_state, display);
			elements.emplace_back(_media_element);
			elements.emplace_back(media_control_style(media_control_element));
		}
		else
		{
			elements.emplace_back(media_control_style(media_control_element));
		}
	}

	// The preview represents the selection, so the commands that act on the selection are drawn
	// with the panel that describes it rather than in a bar of their own.
	const auto priority_end = std::ranges::find(elements, priority_end_element);
	if (priority_end == elements.end())
	{
		_media_priority_elements.clear();
		_media_detail_elements = elements;
	}
	else
	{
		const auto detail_begin = std::next(priority_end);
		_media_priority_elements.assign(elements.begin(), detail_begin);
		_media_detail_elements.assign(detail_begin, elements.end());
	}
	std::swap(_media_elements, elements);
}

void items_view::display_changed()
{
	_pending_media_anchor = {};
	_reset_media_scroll = true;
	_media_scroller.scroll_offset(_host, 0, 0);
	_state.invalidate_view(view_invalid::view_layout);
};

void items_view::line_up(const bool toggle_selection, const bool extend_selection) const
{
	const auto focus = _state.focus_item();

	if (focus)
	{
		auto top = focus->bounds.top_center();

		// scan up looking for something above
		for (int i = 0; i < 100; i++)
		{
			top.y -= 8;

			auto found = _state.item_from_layout_location(top);

			if (found)
			{
				_state.stop_slideshow();
				_state.select(_host, found, toggle_selection, extend_selection, false);
				return;
			}
		}
	}

	_state.select_next(_host, false, toggle_selection, extend_selection);
}

void items_view::line_down(const bool toggle_selection, const bool extend_selection) const
{
	const auto focus = _state.focus_item();

	if (focus)
	{
		auto bottom = focus->bounds.bottom_center();

		// scan looking for something below
		for (int i = 0; i < 100; i++)
		{
			bottom.y += 8;

			auto found = _state.item_from_layout_location(bottom);

			if (found)
			{
				_state.stop_slideshow();
				_state.select(_host, found, toggle_selection, extend_selection, false);
				return;
			}
		}
	}

	_state.select_next(_host, true, toggle_selection, extend_selection);
}

void items_view::items_scroll_popup(view_hover_element& hover, const pointi loc) const
{
	const auto found = scroll_loc_to_item(loc);

	if (found.group && found.item)
	{
		const auto elements = std::make_shared<view_elements>();
		found.group->scroll_tooltip(found.item, elements, _state._async);

		if (!elements->is_empty())
		{
			const auto scroll_bounds = _items_scroller.scroll_bounds();
			const auto top = loc.y;
			const auto left = scroll_bounds.left;
			const auto right = scroll_bounds.right;

			hover.elements = elements;
			hover.window_bounds = recti(left, top, right, top + 16);
			hover.active_bounds = recti(left, loc.y - 1, right, loc.y + 1);
			hover.active_delay = 50;
			hover.horizontal = true;
			hover.preferred_size = 200;
		}
	}
}

group_and_item items_view::scroll_loc_to_item(const pointi loc) const
{
	const auto y = _items_scroller.scrollbar_pos_to_logical(loc.y);

	df::item_group_ptr group;
	df::item_element_ptr item;

	{
		// Groups are stacked top to bottom, so binary search to the boundary and compare the two
		// candidates either side of it instead of walking every group on each mouse move.
		const auto& item_groups = _state.groups();
		const auto centre = [](const df::item_group_ptr& g) { return (g->bounds.top + g->bounds.bottom) / 2; };
		const auto found = std::ranges::lower_bound(item_groups, y, {}, centre);

		if (found != item_groups.end())
		{
			group = *found;
		}

		if (found != item_groups.begin())
		{
			const auto& before = *std::prev(found);
			if (!group || abs(y - centre(before)) <= abs(y - centre(group))) group = before;
		}
	}

	if (group)
	{
		// Items are ordered top to bottom, so only the row before and the row after y can hold the
		// closest item. Binary search to that boundary instead of walking the whole group.
		const auto& items = group->items();
		const auto centre = [](const df::item_element_ptr& i) { return (i->bounds.top + i->bounds.bottom) / 2; };
		const auto found = std::ranges::lower_bound(items, y, {}, centre);

		auto first = found;

		if (first != items.begin())
		{
			--first;
			const auto row = centre(*first);
			while (first != items.begin() && centre(*std::prev(first)) == row) --first;
		}

		auto last = found;

		if (last != items.end())
		{
			const auto row = centre(*last);
			while (last != items.end() && centre(*last) == row) ++last;
		}

		auto distance = 0;

		for (auto it = first; it != last; ++it)
		{
			const auto d = abs(y - centre(*it));

			if (!item || distance > d || (distance == d && (*it)->has_thumb() && !item->has_thumb()))
			{
				item = *it;
				distance = d;
			}
		}
	}

	return {group, item};
}
