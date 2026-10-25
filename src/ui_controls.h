// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Custom UI controls and visual elements. Implements command links, rating controls,
// photo viewers, video controls, scrubbers, hex displays, and other interactive UI components.

#pragma once

#include "app_commands.h"
#include "ui_controllers.h"
#include "ui_elements.h"
#include "ui_text_edit.h"
#include "model_display.h"
#include "libfiles/commodore.h"

class display_state_t;

inline recti comparison_divider_device_bounds(const recti logical_bounds, const pointi element_offset)
{
	return logical_bounds.offset(element_offset);
}

inline int slider_value_to_track_x(const recti track_bounds, const int value, const int min_value, const int max_value)
{
	const auto range = std::max(1, max_value - min_value);
	const auto clamped = std::clamp(value, min_value, max_value);
	return track_bounds.left + df::round(static_cast<double>(clamped - min_value) * track_bounds.width() / range);
}

inline int slider_track_x_to_value(const recti track_bounds, const int x, const int min_value, const int max_value,
                                   const int current_value)
{
	const auto range = std::max(1, max_value - min_value);
	const auto target = std::clamp(x, track_bounds.left, track_bounds.right);
	const auto width = std::max(1, track_bounds.width());
	const auto raw = static_cast<double>(target - track_bounds.left) * range / width;
	const auto distance_to = [&](const int value)
	{
		return std::abs(slider_value_to_track_x(track_bounds, value, min_value, max_value) - target);
	};

	// Start from the proportional value so both ends stay reachable when several values share a pixel,
	// and let a neighbour win only when its painted thumb is strictly closer.
	const auto nearest = std::clamp(min_value + df::round(raw), min_value, max_value);
	auto best = nearest;
	auto best_distance = distance_to(nearest);

	for (const auto offset : {-1, 1, -2, 2})
	{
		const auto value = nearest + offset;
		if (value < min_value || value > max_value) continue;
		const auto distance = distance_to(value);
		if (distance < best_distance)
		{
			best = value;
			best_distance = distance;
		}
	}

	// Pressing the thumb where it is drawn keeps its value.
	const auto current = std::clamp(current_value, min_value, max_value);
	if (distance_to(current) <= best_distance) best = current;
	return best;
}

class slider_element final : public std::enable_shared_from_this<slider_element>, public view_element
{
	std::function<int()> _value;
	std::function<void(int)> _changed;
	// Fired once when the drag releases. A caller whose change is expensive settles here instead of
	// on every pixel of movement.
	std::function<void()> _committed;
	int _min = 0;
	int _max = 0;
	const text_t& _tooltip;
	mutable recti _slider_bounds;
	mutable recti _track_bounds;
	mutable int _handle_size = 12;
	mutable int _track_height = 4;

	static recti calc_track_bounds(const recti slider_bounds, const int handle_size, const int track_height)
	{
		const auto center_y = slider_bounds.center().y;
		return {
			slider_bounds.left + handle_size / 2, center_y - track_height / 2,
			slider_bounds.right - handle_size / 2, center_y + (track_height + 1) / 2
		};
	}

public:
	slider_element(std::function<int()> value, std::function<void(int)> changed, const int min, const int max,
	               const text_t& tooltip, std::function<void()> committed = {}) :
		view_element(view_element_style::can_invoke | view_element_style::has_tooltip), _value(std::move(value)),
		_changed(std::move(changed)), _committed(std::move(committed)), _min(min), _max(max), _tooltip(tooltip)
	{
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		_handle_size = std::max(mc.padding2, df::round(12 * mc.scale_factor));
		_track_height = std::max(2, df::round(4 * mc.scale_factor));
		return {width_limit, std::max(mc.scroll_width, mc.text_line_height(ui::style::font_face::dialog))};
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		view_element::layout(mc, bounds_in, positions);
		_handle_size = std::max(mc.padding2, df::round(12 * mc.scale_factor));
		_track_height = std::max(2, df::round(4 * mc.scale_factor));
		_slider_bounds = bounds;
		_track_bounds = calc_track_bounds(_slider_bounds, _handle_size, _track_height);
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = _slider_bounds = bounds.offset(element_offset);
		_handle_size = std::max(dc.padding2, df::round(12 * dc.scale_factor));
		_track_height = std::max(2, df::round(4 * dc.scale_factor));
		const auto center_y = logical_bounds.center().y;
		_track_bounds = calc_track_bounds(logical_bounds, _handle_size, _track_height);

		const auto value = std::clamp(_value(), _min, _max);
		const auto handle_x = slider_value_to_track_x(_track_bounds, value, _min, _max);
		const auto handle_bounds = recti(handle_x - _handle_size / 2, center_y - _handle_size / 2,
		                                 handle_x + (_handle_size + 1) / 2,
		                                 center_y + (_handle_size + 1) / 2);
		const auto is_hover = is_style_bit_set(view_element_style::hover);
		const auto is_tracking = is_style_bit_set(view_element_style::tracking);

		dc.draw_rounded_rect(_track_bounds, ui::color(0, dc.colors.alpha / 3.33f), dc.padding1);
		dc.draw_rounded_rect(handle_bounds,
		                     view_handle_color(false, is_hover, is_tracking, dc.frame_has_focus, true).aa(
			                     dc.colors.alpha), dc.padding1);
	}

	void hover(interaction_context& ic) override
	{
		const auto was_tracking = is_style_bit_set(view_element_style::tracking);
		if (was_tracking != ic.tracking) ic.invalidate_view = true;

		// A controller being torn down says the pointer is gone by passing {-1,-1}. That is a
		// leave, not a drag off the left edge - and clamping it lands on _min, so a slider or a
		// scrubber jumped to the start as the control that owned it was replaced, taking the
		// playback position or the setting with it.
		const auto pointer_left = ic.loc.x < 0 && ic.loc.y < 0;

		if ((was_tracking || ic.tracking) && !pointer_left && !_slider_bounds.is_empty())
		{
			const auto slider_bounds = bounds.offset(ic.element_offset);
			const auto track_bounds = calc_track_bounds(slider_bounds, _handle_size, _track_height);
			const auto value = slider_track_x_to_value(track_bounds, ic.loc.x, _min, _max, _value());
			if (value != _value()) _changed(value);
		}

		set_style_bit(view_element_style::tracking, ic.tracking);

		// Last, because settling may replace the element tree that owns this slider.
		if (was_tracking && !ic.tracking && _committed) _committed();
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		hover.elements->add(std::make_shared<text_element>(_tooltip));
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

// A toolbar command drawn as a transparent link, matching the icon links already used on group
// titles. Unlike link_element it follows the command's own visible, enable, checked and toolbar
// text state and opens the command's menu in place, so a command reads and behaves the same
// wherever it is placed on the render surface rather than only inside a toolbar band.
class command_link_element final : public std::enable_shared_from_this<command_link_element>, public view_element
{
	ui::command_ptr _command;
	std::string _toolbar_text;
	icon_index _icon = icon_index::none;
	mutable recti _device_bounds;

public:
	// Returns 0-1000 to draw a progress fill along the base of the link, or -1 for none. Only a
	// command that runs a timed mode sets this, so the control that reports progress is the
	// control that ends it.
	std::function<int()> progress;

	explicit command_link_element(ui::command_ptr command,
	                              const view_element_options& style_in = {}) :
		view_element(style_in), _command(std::move(command))
	{
		padding = {4, 4};
		sync_command_state();
	}

	const ui::command_ptr& command() const
	{
		return _command;
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		if (!_command->visible) return {};

		const auto cy = mc.text_line_height(ui::style::font_face::dialog);
		auto cx = 0;

		if (_command->icon != icon_index::none) cx += cy;

		const auto text = label();

		if (!text.empty())
		{
			cx += mc.padding1 + mc.measure_text(text, ui::style::font_face::dialog,
			                                    ui::style::text_style::single_line, width_limit).cx;
		}

		return {std::min(std::max(cx, 1), width_limit), cy};
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		if (!_command->visible) return;

		const auto logical_bounds = bounds.offset(element_offset);
		_device_bounds = logical_bounds;

		render_background(dc, element_offset);

		const auto alpha = (_command->enable ? 1.0f : 0.35f) * dc.colors.alpha;
		const auto clr = ui::color(link_foreground_color(), alpha);
		const auto text = label();
		auto text_bounds = logical_bounds;

		if (progress)
		{
			const auto pos = progress();

			if (pos >= 0)
			{
				auto track = logical_bounds;
				track.top = track.bottom - df::round(3 * dc.scale_factor);
				dc.draw_rect(track, ui::color(ui::style::color::group_background, alpha * 0.7f));
				track.right = track.left + df::mul_div(std::min(pos, 1000), track.width(), 1000);
				dc.draw_rect(track, ui::color(ui::style::color::view_selected_background, alpha));
			}
		}

		if (_command->icon != icon_index::none)
		{
			auto icon_bounds = logical_bounds;
			if (!text.empty()) icon_bounds.right = icon_bounds.left + logical_bounds.height();
			xdraw_icon(dc, _command->icon, icon_bounds, clr, {});
			text_bounds.left = icon_bounds.right + dc.padding1;
		}

		if (!text.empty())
		{
			dc.draw_text(text, text_bounds, ui::style::font_face::dialog,
			             _command->icon != icon_index::none
				             ? ui::style::text_style::single_line
				             : ui::style::text_style::single_line_center, clr, {});
		}
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		if (_command->opaque.type() == typeid(commands))
		{
			hover.id = std::any_cast<commands>(_command->opaque);
			hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
		}
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::update_command_state)
		{
			const auto previous_style = style;
			const auto previous_icon = _icon;
			const auto relayout = _toolbar_text != _command->toolbar_text;
			sync_command_state();

			if (event.host && (relayout || previous_icon != _icon || previous_style != style))
			{
				event.host->invalidate_view(relayout ? view_invalid::view_layout : view_invalid::view_redraw);
			}
		}
		else if (event.type == view_element_event_type::invoke)
		{
			invoke_command(event.host);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		_device_bounds = bounds.offset(element_offset);
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

private:
	// The label is only drawn when the command supplies toolbar text, or when it has no icon at
	// all; commands that carry a count (the media type filters) therefore keep showing it.
	std::string_view label() const
	{
		if (!_command->toolbar_text.empty()) return _command->toolbar_text;
		return _command->icon == icon_index::none ? std::string_view(_command->text) : std::string_view{};
	}

	void sync_command_state()
	{
		_toolbar_text = _command->toolbar_text;
		_icon = _command->icon;
		is_visible(_command->visible);
		set_style_bit(view_element_style::can_invoke, _command->enable);
		set_style_bit(view_element_style::has_tooltip, true);
		set_style_bit(view_element_style::checked, _command->checked);
	}

	// A disabled command still reports why it cannot run, so it dims rather than disappearing.
	void invoke_command(const view_host_ptr& host) const
	{
		if (!_command->enable) return;

		const auto menu = _command->menu ? _command->menu() : std::vector<ui::command_ptr>{};

		if (!menu.empty())
		{
			if (host) host->track_menu(_device_bounds, menu);
		}
		else if (_command->invoke)
		{
			_command->invoke();
		}
	}
};

using command_link_element_ptr = std::shared_ptr<command_link_element>;

static std::string_view orientation_to_string(const ui::orientation& o) noexcept
{
	switch (o)
	{
	case ui::orientation::top_left: return tt.orientation_top_left;
	case ui::orientation::top_right: return tt.orientation_top_right;
	case ui::orientation::bottom_right: return tt.orientation_bottom_right;
	case ui::orientation::bottom_left: return tt.orientation_bottom_left;
	case ui::orientation::left_top: return tt.orientation_left_top;
	case ui::orientation::right_top: return tt.orientation_right_top;
	case ui::orientation::right_bottom: return tt.orientation_right_bottom;
	case ui::orientation::left_bottom: return tt.orientation_left_bottom;
	default: break;
	}

	return {};
}

namespace ui
{
	std::vector<recti> layout_collage(recti draw_bounds, const std::vector<sizei>& dimensions);
}

class edit_element;
using edit_element_ptr = std::shared_ptr<edit_element>;

// A window-free single line text edit drawn by the view renderer.
//
// The element owns the editing model, the caret metrics and the horizontal scroll offset.
// Metrics are built in the measure/layout phase and rebuilt only when the text or the display
// scale changes, so hit testing is valid straight after layout() and a repaint normally measures
// no text at all. Both properties matter: the previous per-frame measurement was quadratic in the
// length of the text and caret hit testing silently depended on a preceding paint.
class edit_element final : public view_element, public std::enable_shared_from_this<edit_element>
{
public:
	using changed_fn = std::function<void(std::string_view)>;
	using action_fn = std::function<void()>;
	using focus_fn = std::function<void(bool)>;

	ui::single_line_edit_model model;

	// Placeholder drawn while the text is empty.
	const text_t* cue = nullptr;

	// Optional glyph drawn at the leading edge, e.g. the scope of the current search.
	icon_index icon = icon_index::none;

	// Overrides the interior fill; empty uses the theme edit background. Owners animate this to
	// signal state (a running search) without adding a separate control.
	std::optional<ui::color32> background;

	// Raised whenever the text changes as a result of user input.
	changed_fn changed;
	// Raised when the user accepts the text (Enter).
	action_fn commit;
	// Raised when the user abandons the edit (Escape); the model is reverted first.
	action_fn cancel;
	// Raised when the focused state changes.
	focus_fn focus_changed;
	// Asked to give this edit the input focus; the owner decides what else to blur.
	action_fn request_focus;

	edit_element() noexcept : view_element(view_element_style::can_invoke)
	{
	}

	const std::string& text() const noexcept { return model.text(); }

	void text(std::string value)
	{
		if (model.text() == value) return;
		model.text(std::move(value));
		_metrics_valid = false;
		_scroll_x = 0;
	}

	bool focused() const noexcept { return _focused; }

	// Takes focus and selects the whole text, matching the behavior of a freshly focused
	// Windows edit control. Returns true when the focused state changed.
	bool focus()
	{
		if (_focused) return false;
		_focused = true;
		model.begin_edit();
		model.select_all();
		reset_caret();
		if (focus_changed) focus_changed(true);
		return true;
	}

	bool blur()
	{
		if (!_focused) return false;
		_focused = false;
		if (focus_changed) focus_changed(false);
		return true;
	}

	void reset_caret()
	{
		const auto interval = platform::caret_blink_time();
		_caret_phase = interval == 0 ? 0 : platform::tick_count() / interval;
		_caret_visible = true;
	}

	// Advances the caret blink. Returns true when the edit needs repainting.
	bool update_caret()
	{
		if (!_focused) return false;

		const auto interval = platform::caret_blink_time();

		if (interval == 0)
		{
			const auto was_hidden = !_caret_visible;
			_caret_visible = true;
			return was_hidden;
		}

		const auto phase = platform::tick_count() / interval;
		if (phase == _caret_phase) return false;
		_caret_phase = phase;
		_caret_visible = (phase & 1) == 0;
		return true;
	}

	// Maps a device x coordinate onto the nearest character boundary.
	size_t caret_from_x(const int x) const
	{
		if (_widths.size() < 2) return model.text().size();

		const auto target = x - _inner.left + _scroll_x;
		const auto found = std::ranges::lower_bound(_widths, target);
		if (found == _widths.begin()) return _offsets.front();
		if (found == _widths.end()) return _offsets.back();

		const auto high = static_cast<size_t>(found - _widths.begin());
		const auto low = high - 1;
		return (target - _widths[low] <= _widths[high] - target) ? _offsets[low] : _offsets[high];
	}

	// Returns true when the key was consumed by the edit.
	bool key_down(const char32_t key, const ui::key_state keys)
	{
		if (!_focused) return false;

		auto text_changed = false;

		if (keys.control && key == 'A') model.select_all();
		else if (keys.control && key == 'C')
		{
			// Nothing selected is nothing to copy. Sending it anyway replaced whatever the user had
			// put on the clipboard with an empty string, which is a loss they did not ask for and
			// cannot undo.
			const auto selected = model.selected_text();
			if (!selected.empty()) platform::set_clipboard(selected);
		}
		else if (keys.control && key == 'X')
		{
			const auto selected = model.selected_text();
			if (!selected.empty())
			{
				text_changed = model.cut_to_clipboard([](const std::string_view text)
				{
					return platform::set_clipboard(text);
				});
			}
		}
		else if (keys.control && key == 'V')
		{
			text_changed = model.paste_from_clipboard([] { return platform::clipboard_text(); });
		}
		else if (keys.control && key == 'Z')
		{
			keys.shift ? model.redo() : model.undo();
			text_changed = true;
		}
		else if (keys.control && key == 'Y')
		{
			model.redo();
			text_changed = true;
		}
		else if (key == keys::LEFT) keys.control ? model.move_word_left(keys.shift) : model.move_left(keys.shift);
		else if (key == keys::RIGHT) keys.control ? model.move_word_right(keys.shift) : model.move_right(keys.shift);
		else if (key == keys::HOME) model.move_home(keys.shift);
		else if (key == keys::END) model.move_end(keys.shift);
		else if (key == keys::BACK)
		{
			keys.control ? model.backspace_word() : model.backspace();
			text_changed = true;
		}
		else if (key == keys::DEL)
		{
			keys.control ? model.delete_word() : model.delete_forward();
			text_changed = true;
		}
		else if (key == keys::ESCAPE)
		{
			model.cancel_edit();
			_metrics_valid = false;
			notify_changed();
			blur();
			if (cancel) cancel();
			return true;
		}
		else if (key == keys::RETURN)
		{
			blur();
			if (commit) commit();
			return true;
		}
		else if (key == keys::TAB)
		{
			blur();
			return true;
		}
		else return false;

		if (text_changed)
		{
			_metrics_valid = false;
			notify_changed();
		}

		reset_caret();
		return true;
	}

	bool text_input(const std::string_view input)
	{
		if (!_focused) return false;
		model.insert(input);
		_metrics_valid = false;
		reset_caret();
		notify_changed();
		return true;
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {width_limit, mc.text_line_height(ui::style::font_face::dialog) + mc.padding2 * 2};
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;
		update_inner(mc);
		update_metrics(mc);
		scroll_caret_into_view();
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		update_inner(dc);
		update_metrics(dc);
		scroll_caret_into_view();

		const auto draw_bounds = bounds.offset(element_offset);
		const auto inner = _inner.offset(element_offset);
		const auto text_color = ui::color(ui::style::color::edit_text, dc.colors.alpha);

		draw_frame(dc, draw_bounds);

		if (icon != icon_index::none)
		{
			xdraw_icon(dc, icon, _icon_bounds.offset(element_offset), text_color.aa(0.7f), {});
		}

		dc.clip_bounds(inner);

		if (model.has_selection())
		{
			const recti selection{
				inner.left + offset_x(model.selection_start()) - _scroll_x, inner.top,
				inner.left + offset_x(model.selection_end()) - _scroll_x, inner.bottom
			};
			dc.draw_rect(selection, ui::color(ui::style::color::dialog_selected_background, dc.colors.alpha));
		}

		const auto& value = model.text();

		if (value.empty())
		{
			if (cue && !cue->sv().empty())
			{
				dc.draw_text(cue->sv(), inner, ui::style::font_face::dialog, ui::style::text_style::single_line,
				             text_color.aa(0.55f), {});
			}
		}
		else
		{
			dc.draw_text(value, inner.offset(-_scroll_x, 0), ui::style::font_face::dialog,
			             ui::style::text_style::single_line, text_color, {});
		}

		if (_focused && _caret_visible)
		{
			const auto caret_x = inner.left + offset_x(model.caret()) - _scroll_x;
			dc.draw_rect({caret_x, inner.top + dc.padding1, caret_x + 1, inner.bottom - dc.padding1}, text_color);
		}

		dc.restore_clip();
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::dpi_changed)
		{
			_metrics_valid = false;
		}
	}

private:
	// Caret metrics: _offsets holds every utf-8 character boundary (including 0 and size())
	// and _widths the matching prefix width. Both are rebuilt only when the text or the
	// display scale changes.
	mutable std::vector<size_t> _offsets;
	mutable std::vector<int> _widths;
	mutable std::string _metrics_text;
	mutable double _metrics_scale = 0.0;
	mutable bool _metrics_valid = false;
	mutable int _scroll_x = 0;
	mutable recti _inner;
	mutable recti _icon_bounds;
	bool _focused = false;
	bool _caret_visible = true;
	uint64_t _caret_phase = 0;

	void notify_changed() const
	{
		if (changed) changed(model.text());
	}

	// The text area excludes the optional leading icon. Recomputed on every layout and paint so a
	// scope change, which does not resize the edit, cannot leave hit testing on stale metrics.
	void update_inner(ui::measure_context& mc) const
	{
		auto inner = bounds.inflate(-mc.padding2, 0);

		if (icon == icon_index::none)
		{
			_icon_bounds = {};
		}
		else
		{
			const auto icon_width = mc.text_line_height(ui::style::font_face::icons);
			_icon_bounds = {inner.left, inner.top, inner.left + icon_width, inner.bottom};
			inner.left = std::min(_icon_bounds.right + mc.padding1, inner.right);
		}

		_inner = inner;
	}

	void update_metrics(ui::measure_context& mc) const
	{
		const auto& value = model.text();

		if (_metrics_valid && _metrics_scale == mc.scale_factor && _metrics_text == value) return;

		_metrics_valid = true;
		_metrics_scale = mc.scale_factor;
		_metrics_text = value;

		_offsets.clear();
		_offsets.emplace_back(0);

		for (size_t offset = 0; offset < value.size();)
		{
			++offset;
			while (offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xc0) == 0x80) ++offset;
			_offsets.emplace_back(offset);
		}

		_widths.clear();
		_widths.reserve(_offsets.size());

		auto layout = mc.create_text_layout(ui::style::font_face::dialog);
		if (layout)
		{
			layout->update(value, ui::style::text_style::single_line);
			_widths = layout->offset_xs(_offsets, 10000);
			if (_widths.size() == _offsets.size()) return;
		}

		_widths.clear();
		for (const auto offset : _offsets)
			_widths.emplace_back(offset == 0
				                     ? 0
				                     : mc.measure_text(std::string_view(value).substr(0, offset),
				                                       ui::style::font_face::dialog,
				                                       ui::style::text_style::single_line, 10000).cx);
	}

	int offset_x(const size_t offset) const
	{
		if (_widths.empty()) return 0;
		const auto found = std::ranges::lower_bound(_offsets, offset);
		const auto index = found == _offsets.end()
			                   ? _offsets.size() - 1
			                   : static_cast<size_t>(found - _offsets.begin());
		return _widths[index];
	}

	void scroll_caret_into_view() const
	{
		const auto width = std::max(0, _inner.width());
		const auto caret_x = offset_x(model.caret());
		const auto text_width = _widths.empty() ? 0 : _widths.back();

		if (caret_x - _scroll_x > width) _scroll_x = caret_x - width;
		if (caret_x < _scroll_x) _scroll_x = caret_x;
		_scroll_x = std::clamp(_scroll_x, 0, std::max(0, text_width - width));
	}

	void draw_frame(ui::draw_context& dc, const recti draw_bounds) const
	{
		const auto border_width = _focused ? std::max(2, df::round(2.6 * dc.scale_factor)) : 1;
		const auto border = _focused
			                    ? ui::color(ui::style::color::dialog_selected_background, dc.colors.alpha)
			                    : ui::color(ui::average(ui::style::color::toolbar_background,
			                                            ui::style::color::edit_text), dc.colors.alpha);
		dc.draw_rounded_rect(draw_bounds, border, dc.padding1);
		dc.draw_rounded_rect(draw_bounds.inflate(-border_width),
		                     ui::color(background.value_or(ui::style::color::edit_background), dc.colors.alpha),
		                     std::max(1, dc.padding1 - border_width / 2));
	}
};

// Mouse interaction for edit_element: click to place the caret, drag or shift-click to extend
// the selection and double click to select a word. The first click focuses the edit and keeps
// the select-all made by edit_element::focus().
class edit_element_controller final : public view_controller
{
	edit_element_ptr _edit;
	size_t _anchor = 0;
	bool _tracking = false;

public:
	edit_element_controller(const view_host_ptr& host, edit_element_ptr edit, const recti bounds) :
		view_controller(host, bounds), _edit(std::move(edit))
	{
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::text_select;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_tracking = true;

		const auto was_focused = _edit->focused();
		if (_edit->request_focus) _edit->request_focus();

		if (!was_focused)
		{
			_anchor = _edit->model.anchor();
		}
		else if (keys.shift)
		{
			_anchor = _edit->model.anchor();
			_edit->model.select(_anchor, _edit->caret_from_x(loc.x));
		}
		else
		{
			_anchor = _edit->caret_from_x(loc.x);
			_edit->model.select(_anchor, _anchor);
		}

		_edit->reset_caret();
		_host->frame()->invalidate(_bounds);
	}

	void on_mouse_move(const pointi loc) override
	{
		if (!_tracking) return;
		_edit->model.select(_anchor, _edit->caret_from_x(loc.x));
		_host->frame()->invalidate(_bounds);
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		_tracking = false;
	}

	void on_mouse_left_button_double_click(const pointi loc, const ui::key_state keys) override
	{
		_edit->model.select_word(_edit->caret_from_x(loc.x));
		_anchor = _edit->model.anchor();
		_edit->reset_caret();
		_host->frame()->invalidate(_bounds);
	}
};

inline view_controller_ptr edit_element::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                                  const pointi element_offset,
                                                                  hit_test_context& ctx)
{
	const auto draw_bounds = bounds.offset(element_offset);
	if (!is_visible() || !draw_bounds.contains(loc)) return nullptr;
	return std::make_shared<edit_element_controller>(host, shared_from_this(), draw_bounds);
}

class rating_control final : public std::enable_shared_from_this<rating_control>, public view_element
{
	view_state& _state;

	mutable int _icon_cxy = 0;
	int _hover_rating = 0;
	int last_hover_rating = 0;
	const bool show_accelerator = false;
	const df::item_element_ptr _item;
	const bool _can_edit;

public:
	rating_control(view_state& s, df::item_element_ptr i, const bool show_accelerator,
	               const view_element_options& style_in) noexcept : view_element(style_in), _state(s),
	                                                                show_accelerator(show_accelerator),
	                                                                _item(std::move(i)),
	                                                                _can_edit([this]
	                                                                {
		                                                                const df::item_set items = {{_item}};
		                                                                return items.can_process(
			                                                                df::process_items_type::can_save_metadata,
			                                                                false, {}).success();
	                                                                }())
	{
		style |= view_element_style::has_tooltip;
		if (_can_edit) style |= view_element_style::can_invoke;

		if (s.search().is_match(prop::rating, _item->rating()))
		{
			style |= view_element_style::important;
		}

		update_background_color();
	}

	int displayed_rating() const
	{
		if (_item)
		{
			const auto md = _item->metadata();

			if (md && md->rating != 0)
			{
				return md->rating;
			}
		}

		return 0;
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto bg = calc_background_color(dc);
		const auto is_hover = is_style_bit_set(view_element_style::hover);
		const auto rating = is_hover ? last_hover_rating : displayed_rating();
		const auto clr = ui::color(dc.colors.foreground, _can_edit ? dc.colors.alpha : dc.colors.alpha / 4.0f);

		std::string text;

		for (auto i = 0; i < 5; i++)
		{
			text += icon_to_utf8(i < rating ? icon_index::star_solid : icon_index::star);
		}

		dc.draw_text(text, logical_bounds, ui::style::font_face::icons,
		             ui::style::text_style::single_line_center, clr, bg);
	}

	// The bubble states both grading values, because Reject and the stars share one metadata field
	// and a user who cannot see that will not understand why a star cleared their Reject mark.
	void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		_icon_cxy = mc.icon_cxy;
		return {mc.icon_cxy * 5, mc.icon_cxy};
	}

	void hover(interaction_context& ic) override
	{
		const auto logical_bounds = bounds.offset(ic.element_offset);
		const auto hovering = logical_bounds.contains(ic.loc);
		const auto rating = hovering ? to_rating(logical_bounds, ic.loc) : 0;

		if (_hover_rating != rating)
		{
			_hover_rating = rating;
			ic.invalidate_view = true;
		}

		last_hover_rating = _hover_rating;
	}

	static int to_rating(const recti rating_bounds, const pointi loc)
	{
		// Clamped: the cells are whole divisions of the width, so the pixels past the last full cell
		// - and the inclusive right edge - divided out to a sixth star no rating has.
		return std::clamp((loc.x - rating_bounds.left) / std::max(1, rating_bounds.width() / 5) + 1, 1, 5);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	void dispatch_event(const view_element_event& event) override;
};

// The grading picker for a pending value rather than an item. The batch metadata surface edits a
// number that has not been written anywhere yet, so it cannot reuse rating_control, but the user
// must still see the same vocabulary - Reject plus five stars - that grading shows everywhere else.
class rating_edit_control final : public std::enable_shared_from_this<rating_edit_control>, public view_element
{
	int& _val;
	std::function<void(int)> _changed;
	mutable int _icon_cxy = 0;
	int _hover_cell = -1;

public:
	// Reject occupies the first cell because it is a rating of -1, not a separate field.
	static constexpr int cell_count = 6;

	rating_edit_control(int& val, std::function<void(int)> changed) noexcept : view_element(
		                                                                           view_element_style::can_invoke |
		                                                                           view_element_style::has_tooltip),
	                                                                           _val(val), _changed(std::move(changed))
	{
	}

	static int cell_to_rating(const int cell)
	{
		return cell <= 0 ? -1 : cell;
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		_icon_cxy = mc.icon_cxy;
		return {mc.icon_cxy * cell_count, mc.icon_cxy};
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto bg = calc_background_color(dc);
		const auto cxy = std::max(1, _icon_cxy);
		const auto rating = _hover_cell < 0 ? _val : cell_to_rating(_hover_cell);

		auto reject_bounds = logical_bounds;
		reject_bounds.right = reject_bounds.left + cxy;

		const auto reject_text = icon_to_utf8(rate_label_reject.icon);
		const auto reject_clr = rating < 0
			                        ? ui::color(rate_label_reject.clr, dc.colors.alpha)
			                        : ui::color(dc.colors.foreground, dc.colors.alpha / 3.0f);

		dc.draw_text(reject_text, reject_bounds, ui::style::font_face::icons,
		             ui::style::text_style::single_line_center, reject_clr, bg);

		std::string stars;

		for (auto i = 0; i < 5; i++)
		{
			stars += icon_to_utf8(i < rating ? icon_index::star_solid : icon_index::star);
		}

		auto star_bounds = logical_bounds;
		star_bounds.left = logical_bounds.left + cxy;

		dc.draw_text(stars, star_bounds, ui::style::font_face::icons,
		             ui::style::text_style::single_line_center,
		             ui::color(dc.colors.foreground, dc.colors.alpha), bg);
	}

	void hover(interaction_context& ic) override
	{
		const auto logical_bounds = bounds.offset(ic.element_offset);
		const auto cell = logical_bounds.contains(ic.loc)
			                  ? std::clamp((ic.loc.x - logical_bounds.left) / std::max(1, _icon_cxy), 0,
			                               cell_count - 1)
			                  : -1;

		if (_hover_cell != cell)
		{
			_hover_cell = cell;
			ic.invalidate_view = true;
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	// Clicking the mark that is already set clears it, so every value stays reachable without a
	// separate clear affordance.
	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke && _hover_cell >= 0)
		{
			const auto rating = cell_to_rating(_hover_cell);
			_val = _val == rating ? 0 : rating;
			if (_changed) _changed(_val);
		}
	}

	void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;
};

// The compact grading badge. It shows the mark the item currently carries - a colour label, or
// Reject when that wins - and opens the full vocabulary as a menu on click, so the panel spends one
// icon of width instead of six.
class rate_label_control final : public std::enable_shared_from_this<rate_label_control>, public view_element
{
	view_state& _state;
	const df::item_element_ptr _item;
	const bool show_accelerator = false;
	const bool _can_edit;
	mutable recti _view_bounds;

public:
	// Reject is a rating of -1, so it shares this control with the colour labels rather than the stars.
	static constexpr int entry_count = 6;

	rate_label_control(view_state& s, df::item_element_ptr i, const bool show_accelerator,
	                   const view_element_options& style_in) noexcept : view_element(
		                                                                    style_in | view_element_style::has_tooltip |
		                                                                    view_element_style::can_invoke), _state(s),
	                                                                    _item(std::move(i)),
	                                                                    show_accelerator(show_accelerator),
	                                                                    _can_edit([this]
	                                                                    {
		                                                                    const df::item_set items = {{_item}};
		                                                                    return items.can_process(
			                                                                    df::process_items_type::can_save_metadata,
			                                                                    false, {}).success();
	                                                                    }())
	{
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {mc.icon_cxy, mc.icon_cxy};
	}

	void render(ui::draw_context& dc, pointi element_offset) const override;
	void dispatch_event(const view_element_event& event) override;
	void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

class preview_control final : public std::enable_shared_from_this<preview_control>, public view_element
{
	view_state& _state;
	const texture_state_ptr _ts;
	const bool show_accelerator = false;
	mutable recti _view_bounds;

public:
	preview_control(view_state& s, texture_state_ptr ts, const bool show_accelerator,
	                const view_element_options& style_in) noexcept : view_element(style_in), _state(s),
	                                                                 _ts(std::move(ts)),
	                                                                 show_accelerator(show_accelerator)
	{
		style |= view_element_style::has_tooltip | view_element_style::can_invoke;
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {mc.icon_cxy, mc.icon_cxy};
	}

	void render(ui::draw_context& dc, pointi element_offset) const override;
	void dispatch_event(const view_element_event& event) override;
	void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};


class items_dates_control final : public std::enable_shared_from_this<items_dates_control>, public view_element
{
	view_state& _state;
	const df::item_element_ptr _item;
	std::string _text;
	df::date_t _shown_date;
	df::date_parts_prop _shown_target = df::date_parts_prop::original;
	bool _invalid = false;
	ui::style::font_face _font = ui::style::font_face::dialog;
	ui::style::text_style _text_style = ui::style::text_style::multiline;

public:
	items_dates_control(view_state& s, df::item_element_ptr i) noexcept : view_element(
		                                                                      view_element_style::has_tooltip |
		                                                                      view_element_style::can_invoke),
	                                                                      _state(s),
	                                                                      _item(std::move(i))
	{
		const auto& search = s.search();

		// Each candidate is matched against the key that names it. The tile can show the
		// capture-first ladder, the Created concept or the file stamp, and a match on one of those
		// is not a match on another. The stamps are UTC instants in the index, so they are converted
		// before being compared or shown, the way the index matcher converts them.
		const auto created_concept = _item->file_or_metadata_created();
		const auto modified_date = _item->file_modified().system_to_local();

		if (search.is_match(prop::created_utc, created_concept))
		{
			style |= view_element_style::important;
			_text = platform::format_date(created_concept);
			_shown_date = created_concept;
			_shown_target = df::date_parts_prop::created;
		}
		else if (search.is_match(prop::modified, modified_date))
		{
			style |= view_element_style::important;
			_text = platform::format_date(modified_date);
			_shown_date = modified_date;
			_shown_target = df::date_parts_prop::modified;
		}
		else
		{
			const auto created_date = _item->media_created();

			if (search.is_match(prop::created_exif, created_date))
			{
				style |= view_element_style::important;
			}

			if (created_date.is_valid())
			{
				_text = platform::format_date(created_date);
				_shown_date = created_date;
				_shown_target = df::date_parts_prop::original;
			}
		}

		if (_text.empty())
		{
			_invalid = true;
		}

		update_background_color();
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto bg = calc_background_color(dc);
		const auto logical_bounds = bounds.offset(element_offset);
		const auto text = _invalid ? tt.invalid.sv() : std::string_view(_text);
		dc.draw_text(text, logical_bounds, _font, _text_style, ui::color(dc.colors.foreground, dc.colors.alpha), bg);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return mc.measure_text(_invalid ? tt.invalid.sv() : std::string_view(_text), _font, _text_style, width_limit);
	}

	void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			// The bubble names one of the three dates, so it opens that one. Asking for the union
			// would return a day the bubble did not show.
			if (_shown_date.is_valid())
			{
				const auto st = _shown_date.date();
				const auto search = df::search_t().day(st.day, st.month, st.year, _shown_target);
				_state.open(event.host, search, {});
			}
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

class pin_control final : public std::enable_shared_from_this<pin_control>, public view_element
{
	view_state& _state;
	const df::item_element_ptr _item;
	const bool show_accelerator = false;

public:
	pin_control(view_state& s, df::item_element_ptr i, const bool show_accelerator,
	            const view_element_options& style_in) noexcept :
		view_element(style_in | view_element_style::has_tooltip | view_element_style::can_invoke),
		_state(s), _item(std::move(i)), show_accelerator(show_accelerator)
	{
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto i = _item;

		if (i)
		{
			const auto logical_bounds = bounds.offset(element_offset);
			const auto is_pin = _state._pin_item == _item;
			const auto icon = is_pin ? icon_index::pinned : icon_index::pin;
			const auto alpha = dc.colors.alpha;
			auto bg = calc_background_color(dc);

			if (is_pin)
			{
				bg.merge(ui::color(ui::style::color::important_background, dc.colors.alpha));
			}

			xdraw_icon(dc, icon, logical_bounds, ui::color(dc.colors.foreground, alpha), bg);
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {mc.icon_cxy, mc.icon_cxy};
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			const auto i = _item;

			if (i)
			{
				if (_state._pin_item == i)
				{
					_state._pin_item.reset();
				}
				else
				{
					_state._pin_item = i;
				}

				_state.invalidate_view(view_invalid::view_redraw | view_invalid::command_state);
			}
		}
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		hover.id = commands::pin_item;
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

// Surfaces that draw the pin badge but have no room for a pin_control still have to let the user
// release the item, so the badge itself is the affordance.
class unpin_badge_controller final : public view_controller
{
	view_state& _state;

public:
	unpin_badge_controller(const view_host_ptr& host, view_state& state, const recti bounds) :
		view_controller(host, bounds), _state(state)
	{
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::link;
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		if (_bounds.contains(loc))
		{
			_state._pin_item.reset();
			_state.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw |
				view_invalid::command_state);
		}
	}

	void popup_from_location(view_hover_element& hover) override
	{
		hover.id = commands::pin_item;
		hover.active_bounds = hover.window_bounds = _bounds;
	}
};

// A one-icon button attached to an item. CRTP so the shared controller_from_location can reach
// the derived type's shared_from_this(); the tooltip and the invoke action stay per-element.
template <typename T>
class item_icon_element : public view_element
{
protected:
	view_state& _state;
	const df::item_element_ptr _item;
	const icon_index _icon;

	item_icon_element(view_state& s, df::item_element_ptr i, const icon_index icon,
	                  const view_element_options& style_in) noexcept :
		view_element(style_in | view_element_style::has_tooltip | view_element_style::can_invoke),
		_state(s), _item(std::move(i)), _icon(icon)
	{
	}

public:
	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto bg = calc_background_color(dc);
		xdraw_icon(dc, _icon, bounds.offset(element_offset), ui::color(dc.colors.foreground, dc.colors.alpha), bg);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {mc.icon_cxy, mc.icon_cxy};
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(static_cast<T&>(*this), host, loc, element_offset, ctx);
	}
};

class unselect_element final : public std::enable_shared_from_this<unselect_element>,
                               public item_icon_element<unselect_element>
{
public:
	unselect_element(view_state& s, df::item_element_ptr i,
	                 const view_element_options& style_in) noexcept :
		item_icon_element(s, std::move(i), icon_index::close, style_in)
	{
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			_state.unselect(event.host, _item);
		}
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		hover.elements->add(make_icon_element(_icon, flex_item::no_break));
		hover.elements->add(std::make_shared<text_element>(str_format(tt.unselect_fmt.sv(), _item->name())));
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}
};

class delete_element final : public std::enable_shared_from_this<delete_element>,
                             public item_icon_element<delete_element>
{
public:
	delete_element(view_state& s, df::item_element_ptr i,
	               const view_element_options& style_in) noexcept :
		item_icon_element(s, std::move(i), icon_index::del, style_in)
	{
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			df::item_set items;
			items.add(_item);
			_state._events.delete_items(items);
		}
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		hover.elements->add(make_icon_element(_icon, flex_item::no_break));
		hover.elements->add(std::make_shared<text_element>(str_format(tt.delete_fmt.sv(), _item->name())));
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}
};

class stream_element final : public std::enable_shared_from_this<stream_element>, public view_element
{
	view_state& _state;
	const df::item_element_ptr _item;
	const av_stream_info _stream;
	ui::style::font_face _font = ui::style::font_face::dialog;
	ui::style::text_style _text_style = ui::style::text_style::multiline;
	std::string _text;
	icon_index _icon = icon_index::none;

public:
	stream_element(view_state& state, df::item_element_ptr i, av_stream_info stream,
	               const int audio_track_number) noexcept : _state(state),
	                                                        _item(std::move(i)), _stream(std::move(stream))
	{
		if (stream.type == av_stream_type::audio)
		{
			style |= view_element_style::has_tooltip | view_element_style::can_invoke;
			_icon = _stream.is_playing ? icon_index::check : icon_index::audio;
			_text = format_audio_stream_name(_stream, audio_track_number);
		}
		else
		{
			_text = _stream.title;
			if (_text.empty()) _text = str_format(tt.stream_name_fmt.sv(), _stream.index);
		}
		set_style_bit(view_element_style::checked, _stream.is_playing);
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto bg = calc_background_color(dc);
		auto text_bounds = logical_bounds;

		if (_icon != icon_index::none)
		{
			auto icon_bounds = logical_bounds;
			icon_bounds.right = icon_bounds.left + logical_bounds.height();
			xdraw_icon(dc, _icon, icon_bounds, ui::color(link_foreground_color(), dc.colors.alpha), bg);
			text_bounds.left = icon_bounds.right + dc.padding1;
		}

		dc.draw_text(_text, text_bounds, _font, _text_style,
		             ui::color(_icon == icon_index::none ? dc.colors.foreground : link_foreground_color(),
		                       dc.colors.alpha), bg);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		sizei result;

		if (!_text.empty())
		{
			result = mc.measure_text(_text, _font, _text_style, width_limit);
			if (_icon != icon_index::none) result.cx += result.cy + mc.padding1;
		}

		return result;
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			if (_stream.type == av_stream_type::audio)
			{
				_state.change_tracks(-1, _stream.index);
			}
			else if (_stream.type == av_stream_type::video)
			{
				_state.change_tracks(_stream.index, -1);
			}
		}
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		hover.elements->add(make_icon_element(icon_index::audio, flex_item::no_break));
		hover.elements->add(std::make_shared<text_element>(_text));
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

class summary_control final : public std::enable_shared_from_this<summary_control>, public view_element
{
	const df::file_group_histogram _summary;

	mutable int _line_height = 0;
	mutable int _col_1_width = 0;
	mutable int _col_2_width = 0;
	mutable int _col_3_width = 0;

	struct entry
	{
		icon_index icon;
		std::string name;
		std::string count;
		std::string size;
		file_group_ref ft;
	};

	std::vector<entry> _lines;

	ui::style::font_face _font = ui::style::font_face::dialog;

public:
	summary_control(const df::file_group_histogram& summary,
	                const view_element_options& style_in) noexcept : view_element(style_in), _summary(summary)
	{
		populate_lines();
	}

	void populate_lines()
	{
		for (auto i = 0; i < file_group::max_count; ++i)
		{
			const auto& c = _summary.counts[i];

			if (c.count > 0)
			{
				const auto* const ft = file_group_from_index(i);
				const auto size = c.size.is_empty() ? std::string{} : df::format_size(c.size);
				const auto text = ft->display_name(c.count > 1);
				const auto num = platform::format_number(str::to_string(c.count));

				_lines.emplace_back(ft->icon, text, num, size, ft);
			}
		}
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::populate)
		{
			_lines.clear();
			populate_lines();
		}
	}

	const int icon_width = 24;
	const int col_padding = 4;

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto cy = logical_bounds.height() - static_cast<int>(_line_height * _lines.size());
		auto y = logical_bounds.top + cy;

		constexpr auto text_style = ui::style::text_style::single_line;
		constexpr auto num_style = ui::style::text_style::single_line_far;
		const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);

		for (const auto& line : _lines)
		{
			const auto clr_text = ui::color(line.ft->text_color(dc.colors.foreground), dc.colors.alpha);

			auto x = logical_bounds.left;
			xdraw_icon(dc, line.icon, recti(x, y, x + icon_width, y + _line_height), clr, {});
			dc.draw_text(line.count, recti(x + icon_width, y, x + _col_1_width, y + _line_height), _font, num_style,
			             clr, {});
			x += _col_1_width + col_padding;
			dc.draw_text(line.name, recti(x, y, x + _col_2_width - col_padding, y + _line_height), _font, text_style,
			             clr_text.emphasize(), {});
			x += _col_2_width + col_padding;
			dc.draw_text(line.size, recti(x, y, x + _col_3_width - col_padding, y + _line_height), _font, num_style,
			             clr, {});
			y += _line_height;
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		_line_height = mc.text_line_height(ui::style::font_face::dialog) + 2;

		_col_1_width = 0;
		_col_2_width = 0;
		_col_3_width = 0;

		for (const auto& line : _lines)
		{
			auto extent = mc.measure_text(line.count, ui::style::font_face::dialog, ui::style::text_style::single_line,
			                              100, _line_height);
			_col_1_width = std::max(extent.cx + icon_width, _col_1_width);

			extent = mc.measure_text(line.name, ui::style::font_face::dialog, ui::style::text_style::single_line, 100,
			                         _line_height);
			_col_2_width = std::max(extent.cx + col_padding, _col_2_width);

			extent = mc.measure_text(line.size, ui::style::font_face::dialog, ui::style::text_style::single_line, 100,
			                         _line_height);
			_col_3_width = std::max(extent.cx + col_padding, _col_3_width);
		}

		// Render steps past each column by a further col_padding, so the width has to carry those two
		// gaps or the right-aligned size column lands outside the element.
		return {
			_col_1_width + _col_2_width + _col_3_width + col_padding * 2,
			static_cast<int>(_line_height * _lines.size())
		};
	}
};

class file_list_control final : public std::enable_shared_from_this<file_list_control>, public view_element
{
	static constexpr int icon_width = 24;
	static constexpr int col_padding = 8;
	static constexpr int col_count = 4;

	mutable int _line_height = 0;
	mutable int _col_widths[col_count] = {};
	ui::style::text_style _text_style[col_count];

	struct entry
	{
		icon_index icon;
		ui::color32 color;
		std::array<std::string, col_count> text;
		mutable std::array<sizei, col_count> extents;
	};

	display_state_ptr _display;
	std::vector<entry> _lines;

	ui::style::font_face _font = ui::style::font_face::dialog;

public:
	file_list_control(display_state_ptr display, const view_element_options& style_in) noexcept :
		view_element(style_in),
		_display(std::move(display))
	{
		populate();
	}

	void populate()
	{
		_lines.clear();
		_lines.reserve(_display->_archive_items.size());

		for (const auto& i : _display->_archive_items)
		{
			const auto is_empty = i.uncompressed_size.is_empty();
			const auto* const ft = is_empty ? file_type::folder : files::file_type_from_name(i.filename);
			const auto color = ft->text_color(ui::style::color::view_text);
			const auto text = i.filename;
			const auto created = prop::format_date(i.created);
			const auto compressed_size = is_empty ? std::string{} : df::format_size(i.compressed_size);
			const auto uncompressed_size = i.uncompressed_size.is_empty()
				                               ? std::string{}
				                               : df::format_size(i.uncompressed_size);

			_lines.emplace_back(ft->icon, color,
			                    std::array<std::string, col_count>{text, created, uncompressed_size, compressed_size},
			                    std::array<sizei, col_count>{});
		}

		_text_style[0] = ui::style::text_style::single_line;
		_text_style[1] = ui::style::text_style::single_line;
		_text_style[2] = ui::style::text_style::single_line_far;
		_text_style[3] = ui::style::text_style::single_line_far;
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::populate)
		{
			populate();
		}
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto min_top = logical_bounds.top - _line_height;
		const auto cy = logical_bounds.height() - static_cast<int>(_line_height * _lines.size());
		auto y = logical_bounds.top + cy;
		const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);

		for (const auto& line : _lines)
		{
			auto x = logical_bounds.left;
			const auto clr_title = ui::color(line.color, dc.colors.alpha);

			xdraw_icon(dc, line.icon, recti(x, y, x + icon_width, y + _line_height), clr, {});
			x += icon_width;

			if (y > min_top && y < logical_bounds.bottom)
			{
				for (auto i = 0; i < col_count; i++)
				{
					const auto cc = i == 0 ? clr_title : clr;
					const auto width = _col_widths[i];
					auto text_bounds = recti(x, y, x + width - col_padding, y + _line_height);

					if (text_bounds.left < logical_bounds.right)
					{
						if (text_bounds.right > logical_bounds.right) text_bounds.right = logical_bounds.right;
						dc.draw_text(line.text[i], text_bounds, _font, _text_style[i], cc, {});
					}

					x += width;
				}
			}

			y += _line_height;
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		_line_height = mc.text_line_height(ui::style::font_face::dialog) + 2;

		for (int& _col_width : _col_widths)
		{
			_col_width = 0;
		}

		for (const auto& line : _lines)
		{
			for (auto i = 0; i < col_count; i++)
			{
				if (line.extents[i].is_empty())
				{
					line.extents[i] = mc.measure_text(line.text[i], ui::style::font_face::dialog,
					                                  ui::style::text_style::single_line, 1000, _line_height);
				}

				const auto width = line.extents[i].cx + col_padding;
				if (width > _col_widths[i]) _col_widths[i] = width;
			}
		}

		auto total_col_width = icon_width;

		for (const int _col_width : _col_widths)
		{
			total_col_width += _col_width;
		}

		if (total_col_width > width_limit)
		{
			const auto diff = total_col_width - width_limit;

			if (_col_widths[0] - diff > width_limit / col_count)
			{
				_col_widths[0] -= diff;
				total_col_width -= diff;
			}
		}

		return {std::min(width_limit, total_col_width), static_cast<int>(_line_height * _lines.size())};
	}
};


class hex_control final : public std::enable_shared_from_this<hex_control>, public view_element
{
	static constexpr int padding = 4;
	static constexpr int byte_step = 4;
	static constexpr int max_bytes_per_line = 32;
	using hex_source = std::variant<display_state_ptr, std::vector<uint8_t>>;
	hex_source _source;

	mutable int _bytes_per_line = 0;
	mutable int _chars_per_line = 0;
	mutable int _line_height = 0;
	mutable int _line_width = 0;

	ui::style::font_face _font = ui::style::font_face::code;

public:
	hex_control(display_state_ptr display, const view_element_options& style_in) noexcept : view_element(style_in),
		_source(std::move(display))
	{
	}

	hex_control(std::vector<uint8_t> data, const view_element_options& style_in) noexcept : view_element(style_in),
		_source(std::move(data))
	{
	}

	struct char_entry
	{
		char c;
		ui::color32 clr;
	};

	static std::array<char_entry, 256> make_char_map()
	{
		std::array<char_entry, 256> result;

		const auto cl = ui::lighten(ui::style::color::view_selected_background, 0.22f);
		const auto ch = ui::lighten(app_colors::duplicate_background, 0.66f);

		for (auto i = 0; i < 256; i++)
		{
			auto c = static_cast<char>(i);
			auto clr = ui::style::color::view_text;

			if (c == 32u || c == u8'\t')
			{
				c = u8' ';
			}
			else if (c < 32)
			{
				c = u8'.';
				clr = cl;
			}
			else if (c >= 127)
			{
				c = u8'.';
				clr = ch;
			}

			result[i] = {c, clr};
		}

		return result;
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		static const auto char_map = make_char_map();
		static constexpr char hex_chars[16] = {
			'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
		};

		const auto bytes = data();
		const auto line_count = calc_line_count(bytes.size);
		const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);

		if (line_count > 0)
		{
			const auto logical_bounds = bounds.offset(element_offset);
			const auto clip_bounds = dc.clip_bounds().intersection(logical_bounds);
			const auto first_line = (clip_bounds.top - logical_bounds.top) / _line_height;
			const auto last_line = first_line + (clip_bounds.height() + _line_height) / _line_height;
			auto x_ascii = 0u;

			std::string line(_chars_per_line, ' ');
			std::vector<ui::text_highlight_t> highlights;
			highlights.reserve(static_cast<size_t>(_bytes_per_line) * 2 + 1);

			const auto left = (logical_bounds.left + logical_bounds.right - _line_width) / 2;
			const auto data_size = static_cast<int>(bytes.size);

			for (auto i = first_line; i < last_line; ++i)
			{
				const auto start_address = i * _bytes_per_line;

				if (start_address < data_size)
				{
					line.assign(line.size(), ' ');

					const auto limit = std::min(_bytes_per_line, data_size - start_address);
					const auto* const line_data = bytes.data + start_address;

					auto x = 0u;
					const auto address = static_cast<uint32_t>(start_address);

					highlights.emplace_back(x, 8, ui::color(dc.colors.foreground, dc.colors.alpha * 0.77f));

					line[x++] = hex_chars[address >> 28 & 0xF];
					line[x++] = hex_chars[address >> 24 & 0xF];
					line[x++] = hex_chars[address >> 20 & 0xF];
					line[x++] = hex_chars[address >> 16 & 0xF];
					line[x++] = hex_chars[address >> 12 & 0xF];
					line[x++] = hex_chars[address >> 8 & 0xF];
					line[x++] = hex_chars[address >> 4 & 0xF];
					line[x++] = hex_chars[address >> 0 & 0xF];

					x += 2;

					for (auto j = 0; j < limit; ++j)
					{
						const auto byte = line_data[j];
						const auto cc = char_map[byte & 0xff].clr;

						if (cc != dc.colors.foreground)
						{
							highlights.emplace_back(x, 2, ui::color(cc, dc.colors.alpha));
						}

						line[x] = hex_chars[(byte & 0xF0) >> 4];
						++x;
						line[x] = hex_chars[(byte & 0x0F) >> 0];
						x += j % 8 == 7 ? 3 : 2;
					}

					// always start ascii part on same column
					if (x > x_ascii) x_ascii = x;
					x = x_ascii;

					for (auto j = 0; j < limit; ++j)
					{
						const auto& ce = char_map[line_data[j] & 0xff];
						const auto cc = ce.clr;

						if (cc != dc.colors.foreground)
						{
							highlights.emplace_back(x, 1, ui::color(cc, dc.colors.alpha));
						}

						line[x] = ce.c;
						x += j % 8 == 7 ? 2 : 1;
					}

					const auto y = logical_bounds.top + i * _line_height;
					dc.draw_text(line, highlights, recti(left, y, logical_bounds.right, y + _line_height), _font,
					             ui::style::text_style::single_line, clr, {});
					highlights.clear();
				}
			}
		}
	}

	static constexpr uint32_t calc_chars_per_line(const int bytes_per_line)
	{
		return static_cast<int>(sizeof(uint32_t)) * 2 + 4 + bytes_per_line * 4 + bytes_per_line / 8 * 2;
	}

	df::cspan data() const
	{
		if (const auto display = std::get_if<display_state_ptr>(&_source))
		{
			const auto& bytes = (*display)->_selected_item_data;
			return df::cspan{bytes.data(), bytes.size()};
		}

		const auto& bytes = std::get<std::vector<uint8_t>>(_source);
		return df::cspan{bytes.data(), bytes.size()};
	}

	int calc_line_count(const size_t size) const
	{
		return _bytes_per_line > 0 ? df::round_up(static_cast<int>(size), _bytes_per_line) : 0;
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto bytes = data();
		_line_width = 0;
		_bytes_per_line = 0;
		_chars_per_line = 0;

		const auto payload_width = std::max(byte_step,
		                                    std::min(max_bytes_per_line,
		                                             df::round_up(static_cast<int>(bytes.size), byte_step)));

		for (auto candidate = byte_step; candidate <= payload_width; candidate += byte_step)
		{
			const auto chars = calc_chars_per_line(candidate);
			const auto line = std::string(chars, '0');
			const auto extent = mc.measure_text(line, _font,
			                                    ui::style::text_style::single_line, width_limit + 200);

			if (extent.cx > width_limit && _bytes_per_line > 0) break;

			_bytes_per_line = candidate;
			_chars_per_line = calc_chars_per_line(_bytes_per_line);
			_line_width = extent.cx;
			_line_height = extent.cy + padding;
			if (extent.cx > width_limit) break;
		}

		const auto line_count = calc_line_count(bytes.size);
		return {width_limit, _line_height * line_count};
	}
};


class commodore_disk_control final : public std::enable_shared_from_this<commodore_disk_control>, public view_element
{
	display_state_ptr _display;

	mutable ui::texture_ptr _texture;
	mutable sizei _tex_extent;

	libfiles::c64_listing _listing;
	int _cols = 0;
	int _rows = 0;

	uint32_t c64_blue = ui::rgb(33, 27, 174);
	uint32_t c64_light_blue = ui::rgb(95, 83, 254);

	// Scale that fits the cols*8 x rows*8 listing texture into the available
	// width. Prefer an integer DPI-based scale for crisp pixels, but shrink so
	// the listing always stays inside the border when the panel is narrow.
	double fit_scale(const int inner_w, const double scale_factor) const
	{
		const int tex_w = _cols * 8;
		if (tex_w <= 0) return 1.0;

		double scale = std::max(1, df::round(scale_factor * 2.0));
		if (tex_w * scale > inner_w) scale = static_cast<double>(std::max(8, inner_w)) / tex_w;
		return scale;
	}

public:
	commodore_disk_control(display_state_ptr display, const view_element_options& style_in) noexcept :
		view_element(style_in),
		_display(std::move(display))
	{
		const auto& data = _display->_selected_item_data;
		_listing = libfiles::list_c64_media({data.data(), data.size()});
		_cols = static_cast<int>(_listing.columns);
		_rows = static_cast<int>(_listing.rows.size());
	}

	// texture::is_valid() only answers "not null" - it knows nothing about the device that made it -
	// so an element caching one has to answer this broadcast or it draws a texture the device no
	// longer owns.
	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::free_graphics_resources)
		{
			_texture.reset();
			_tex_extent = {};
		}
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		if (_cols <= 0 || _rows <= 0) return;

		if (!_texture || !_texture->is_valid())
		{
			const auto surface = files::c64_listing_surface(_listing, c64_light_blue, c64_blue);

			if (surface)
			{
				_tex_extent = surface->dimensions();
				_texture = dc.create_texture();
				if (_texture) _texture->update(surface);
			}
		}

		if (!_texture) return;

		const auto logical_bounds = bounds.offset(element_offset);
		const auto inner_w = logical_bounds.width() - dc.padding2 * 4;
		const auto scale = fit_scale(inner_w, dc.scale_factor);
		const auto w = std::max(1, df::round(_cols * 8 * scale));
		const auto h = std::max(1, df::round(_rows * 8 * scale));
		const auto left = logical_bounds.left + (logical_bounds.width() - w) / 2;
		const auto top = logical_bounds.top + dc.padding2 * 2;
		const auto dst = recti(left, top, left + w, top + h);

		const auto bg_clr = ui::color(c64_blue, dc.colors.alpha);
		const auto border_clr = ui::color(c64_light_blue, dc.colors.alpha);
		const auto bg_bounds = dst.inflate(dc.padding2);

		// Point sampling keeps crisp retro pixels at exact integer scales; use
		// bilinear when the listing is shrunk to a fractional scale so it stays
		// smooth and legible instead of dropping pixel rows.
		const bool integer_scale = std::abs(scale - std::round(scale)) < 0.01;
		const auto sampler = integer_scale ? ui::texture_sampler::point : ui::texture_sampler::bilinear;

		dc.draw_rect(bg_bounds, bg_clr);
		dc.draw_border(bg_bounds, bg_bounds.inflate(dc.padding2), border_clr, border_clr);
		dc.draw_texture(_texture, dst, recti(0, 0, _tex_extent.cx, _tex_extent.cy), dc.colors.alpha, sampler);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		if (_cols <= 0 || _rows <= 0) return {width_limit, 0};

		const auto inner_w = width_limit - mc.padding2 * 4;
		const auto scale = fit_scale(inner_w, mc.scale_factor);
		const auto h = std::max(1, df::round(_rows * 8 * scale));

		return {width_limit, h + mc.padding2 * 4};
	}
};

class play_control final : public std::enable_shared_from_this<play_control>, public view_element
{
public:
	view_state& _state;
	display_state_ptr _display;

	play_control(view_state& s, const view_element_options& style_in) noexcept :
		view_element(style_in | view_element_style::has_tooltip | view_element_style::can_invoke),
		_state(s), _display(s.display_state())
	{
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);

		render_background(dc, element_offset);

		const auto icon = _display->is_playing_media() ? icon_index::pause : icon_index::play;
		const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);

		xdraw_icon(dc, icon, logical_bounds, clr, {});
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		// Transport leads the row, so it is drawn larger than the icon controls beside it - but it is
		// the same glyph font, so an unscaled box would clip it once the display scale passes 175%.
		const auto cxy = df::round(32 * mc.scale_factor);
		return {cxy, cxy};
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			// commands::ID_PLAY
			_state.play(event.host);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		hover.id = commands::play;
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}
};

class scrubber_element final : public std::enable_shared_from_this<scrubber_element>, public view_element
{
	display_state_ptr _display;
	std::shared_ptr<av_player> _player;

	// The transport row is [play button][scrubber]. Without grow/shrink the scrubber's natural
	// width would push it past the row's right edge and over the media beneath it.
	static constexpr auto flex_layout = []
	{
		flex_item_layout result;
		result.grow = 1.0f;
		result.shrink = 1.0f;
		return result;
	}();

public:
	scrubber_element(std::shared_ptr<av_player> player, display_state_ptr display) noexcept :
		view_element(view_element_style::has_tooltip | view_element_style::can_invoke | flex_layout),
		_display(std::move(display)),
		_player(std::move(player))
	{
	}

	// The track excludes the elapsed/duration labels. Derived from layout, not from render, so hit
	// testing does not depend on a paint having happened or on which view painted last.
	recti calc_track_bounds(const pointi element_offset) const
	{
		const auto logical_bounds = bounds.offset(element_offset);
		if (logical_bounds.width() <= _display->_time_width * 3) return {};
		return logical_bounds.inflate(-_display->_time_width, 0);
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		render_background(dc, element_offset);

		const auto track_bounds = calc_track_bounds(element_offset);
		_display->_scrubber_width = track_bounds.width();

		if (!track_bounds.is_empty())
		{
			const auto is_tracking = is_style_bit_set(view_element_style::tracking);
			const auto is_hover = is_style_bit_set(view_element_style::hover);
			const auto scrub_bg_clr = ui::color(0, dc.colors.alpha / 3.33f);
			const auto scale1 = df::round(1 * dc.scale_factor);

			auto scrub_bounds = track_bounds;

			if (!is_hover && !is_tracking)
			{
				scrub_bounds.top += dc.padding1;
				scrub_bounds.bottom -= dc.padding1;
			}

			dc.draw_rounded_rect(scrub_bounds, scrub_bg_clr, dc.padding1);

			_display->_loading_alpha_animation.target(_display->_session ? 0.0f : 1.0f);

			const auto max_scrubber_width = scrub_bounds.width() - scale1 * 2;
			const auto media_pos = _display->media_pos() - _display->media_start();
			const auto media_len = std::max(1.0, _display->media_end() - _display->media_start());
			const auto pos = df::round(media_pos * max_scrubber_width / media_len);

			scrub_bounds.left += scale1;
			scrub_bounds.right = scrub_bounds.left + std::clamp(pos, scale1 * 2, max_scrubber_width);
			scrub_bounds.top += scale1;
			scrub_bounds.bottom -= scale1;

			dc.draw_rounded_rect(scrub_bounds,
			                     view_handle_color(false, is_hover, is_tracking, dc.frame_has_focus, true).aa(
				                     dc.colors.alpha), dc.padding1);

			auto time_bounds = logical_bounds;
			time_bounds.right = track_bounds.left;

			auto duration_bounds = logical_bounds;
			duration_bounds.left = track_bounds.right;

			const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);

			dc.draw_text(_display->_time, time_bounds, ui::style::font_face::dialog,
			             ui::style::text_style::single_line_center, clr, {});
			dc.draw_text(_display->_duration, duration_bounds, ui::style::font_face::dialog,
			             ui::style::text_style::single_line_center, clr, {});
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto extent = mc.measure_text("00:00:00", ui::style::font_face::dialog,
		                                    ui::style::text_style::single_line, 200);
		_display->_time_width = extent.cx + mc.padding2;
		// Natural width is the narrowest usable track; flex grow claims the row's spare width.
		const auto natural_width = std::min(width_limit, _display->_time_width * 4);
		return {natural_width, std::max(extent.cy, mc.scroll_width)};
	}

	void hover(interaction_context& ic) override
	{
		const auto track_bounds = calc_track_bounds(ic.element_offset);
		if (track_bounds.is_empty()) return;

		const auto is_tracking = is_style_bit_set(view_element_style::tracking);

		// {-1,-1} is a controller saying the pointer is gone: Escape, or the controller being torn
		// down. That is a leave, not a drag off the left edge - clamping it lands on the start, so a
		// scrub cancelled that way sent playback back to the beginning. A scrub still in progress
		// ends where it is, which also takes the player out of scrubbing.
		if (ic.loc.x < 0 && ic.loc.y < 0)
		{
			if (is_tracking && !ic.tracking)
			{
				set_style_bit(view_element_style::tracking, false);
				ic.invalidate_view = true;

				if (_display->_session)
				{
					_player->seek(_display->_session, _display->media_pos(), false);
				}
			}

			return;
		}

		const auto scrubber_width = track_bounds.width();
		const auto media_start = _display->media_start();
		const auto media_end = _display->media_end();
		const auto media_len = media_end - media_start;
		const auto scrubber_pos = std::clamp(ic.loc.x, track_bounds.left, track_bounds.right) - track_bounds.left;

		if (_display->_hover_scrubber_pos != scrubber_pos)
		{
			_display->_hover_scrubber_pos = scrubber_pos;
			_display->load_seek_preview(scrubber_pos, scrubber_width,
			                            [d = ui_owned(_display->_async, _display)] { d->preview_loaded(); });
		}

		if (is_tracking || ic.tracking || is_tracking != ic.tracking)
		{
			const auto time_pos = media_start + floor(scrubber_pos * media_len / std::max(1, scrubber_width));
			set_style_bit(view_element_style::tracking, ic.tracking);

			if (_display->_session)
			{
				_player->seek(_display->_session, time_pos, ic.tracking);
			}
		}
	}

	void dispatch_event(const view_element_event& event) override
	{
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		const auto& surface = _display->_hover_surface;
		const auto track_bounds = calc_track_bounds(element_offset);

		if (!track_bounds.is_empty() && is_valid(surface))
		{
			const auto x = std::clamp(loc.x, track_bounds.left, track_bounds.right);

			hover.elements->add(std::make_shared<surface_element>(surface, 200, view_element_style::none));
			hover.elements->add(std::make_shared<text_element>(str::format_seconds(df::round(surface->time())),
			                                                   ui::style::font_face::dialog,
			                                                   ui::style::text_style::single_line,
			                                                   flex_item::center |
			                                                   flex_item::new_line));

			hover.window_bounds = track_bounds;
			hover.active_bounds = recti(x, track_bounds.top, x + 1, track_bounds.bottom);
			hover.x_focus = _display->_hover_scrubber_pos;
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		if (calc_track_bounds(element_offset).is_empty()) return {};
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

class group_title_control final : public view_element
{
public:
	struct title_element
	{
		view_element_ptr v;
		mutable sizei extent;
		mutable bool visible = true;

		title_element() = default;

		title_element(view_element_ptr vv) : v(std::move(vv))
		{
		}
	};

	std::vector<title_element> elements;

	group_title_control() noexcept = default;

	group_title_control(const std::string_view title,
	                    const std::vector<view_element_ptr>& other_controls = {}) noexcept
	{
		elements.emplace_back(std::make_shared<text_element>(title, ui::style::font_face::title,
		                                                     ui::style::text_style::multiline,
		                                                     flex_item::grow));
		for (const auto& e : other_controls) elements.emplace_back(e);
	}

	// Elements are only appended while the title is being built, so a size check is enough to keep
	// this in step. Rebuilding it per measure/layout cost an allocation per group per frame.
	const std::vector<view_element_ptr>& children() const
	{
		if (_children.size() != elements.size())
		{
			_children.clear();
			_children.reserve(elements.size());
			for (const auto& e : elements) _children.emplace_back(e.v);
		}

		return _children;
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		render_background(dc, element_offset);

		for (const auto& e : elements)
		{
			if (e.visible)
			{
				e.v->render(dc, element_offset);
			}
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		for (const auto& e : elements)
		{
			if (e.v->flex.grow > 0.0f)
			{
				e.v->flex.shrink = 1.0f;
				e.v->flex.min_size.cx = 64;
			}
		}

		flex_container_layout row;
		row.wrap = flex_wrap::no_wrap;
		row.gap.cx = df::round(mc.padding1 / mc.scale_factor);
		return {width_limit, calc_flex_layout(children(), mc, {width_limit, -1}, row).extent.cy};
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;

		flex_container_layout row;
		row.wrap = flex_wrap::no_wrap;
		row.gap.cx = df::round(mc.padding1 / mc.scale_factor);
		const auto calculated = calc_flex_layout(children(), mc, bounds.extent(), row);
		for (auto i = 0u; i < elements.size(); ++i)
		{
			const auto& element = elements[i];
			const auto child_bounds = calculated.layout_bounds[i].offset(bounds.top_left());
			element.visible = child_bounds.right <= bounds.right;
			if (element.visible) element.v->layout(mc, child_bounds, positions);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		view_controller_ptr result;
		for (const auto& e : elements)
		{
			if (e.visible)
			{
				result = e.v->controller_from_location(host, loc, element_offset, ctx);
				if (result) break;
			}
		}
		return result;
	}

	void dispatch_event(const view_element_event& event) override
	{
		for (const auto& i : elements)
		{
			i.v->dispatch_event(event);
		}
	}

private:
	mutable std::vector<view_element_ptr> _children;
};

class photo_control final : public view_element, public std::enable_shared_from_this<photo_control>
{
public:
	view_state& _state;
	display_state_ptr _display;
	view_host_ptr _host;

	bool _can_pan = false;
	mutable recti _zoom_navigator_bounds;
	mutable recti _zoom_fit_bounds;
	mutable recti _zoom_out_bounds;
	mutable recti _zoom_100_bounds;
	mutable recti _zoom_in_bounds;
	mutable recti _zoom_options_bounds;
	// zoom.md L6: the visible control for the flat/projected state of a declared panorama.
	mutable recti _zoom_projection_bounds;
	mutable uint64_t _zoom_quality_generation = 0;
	view_elements_ptr _zoom_grading_element;

	// design.md: a region drawn on the displayed picture. Held in source space so it survives a
	// resize, a layout change and a zoom, and held on the view state keyed to the item rather than on
	// this control, which is rebuilt for anything that raises view_invalid::media_elements - a sidecar
	// arriving or index progress would otherwise erase what the user just drew. The key is what keeps
	// "a crop region is about one picture" true.
	mutable recti _region_close_bounds;
	mutable recti _region_zoom_bounds;
	mutable recti _region_crop_bounds;

	df::file_path region_item() const
	{
		// _display is non-null for this control's whole lifetime - the constructor reads it - so the
		// question here is only whether it resolves to one picture.
		return _display->is_one() && _display->_item1 ? _display->_item1->path() : df::file_path{};
	}

	rectd region() const
	{
		return _state.drawn_region(region_item());
	}

	bool has_region() const
	{
		// zoom.md: a region lives in source space and is drawn where the picture is. A projected view
		// is not the picture laid out in source space, so the rectangle is hidden rather than drawn
		// somewhere it does not mean anything. It is kept, and returns with the flat pixels.
		return !region().is_empty() && !_display->is_panorama_projected();
	}

	void region(const rectd source_rect)
	{
		_state.drawn_region(region_item(), source_rect);
		_host->invalidate_view(view_invalid::view_redraw | view_invalid::controller);
	}

	void clear_region()
	{
		if (!has_region()) return;
		_state.drawn_region(region_item(), {});
		_state.end_region_drag();
		_host->invalidate_view(view_invalid::view_redraw | view_invalid::controller);
	}

	// Where the whole picture lands on screen at the current scale, which is what the region maps
	// through in both directions.
	rectd image_bounds(const pointi element_offset) const
	{
		if (!_display->_selected_texture1) return {};
		return rectd(_display->_selected_texture1->display_bounds().offset(element_offset));
	}

	sized source_extent() const
	{
		if (!_display->_selected_texture1) return {};
		return sized(_display->_selected_texture1->calc_display_dimensions());
	}

	recti region_bounds(const pointi element_offset) const
	{
		if (!has_region()) return {};
		return df::source_rect_to_client(region(), image_bounds(element_offset), source_extent()).round();
	}

	// A drag replaces whatever was there: two rectangles at once would be two answers to a question
	// that has one.
	void begin_region_drag()
	{
		_state.begin_region_drag(region_item(), true);
		_host->invalidate_view(view_invalid::view_redraw | view_invalid::controller);
	}

	// Moving keeps the rectangle; only the buttons go, because the pointer is over them. They are
	// painted, so their leaving is a redraw the drag itself has to ask for.
	void begin_region_move()
	{
		_state.begin_region_drag(region_item(), false);
		_host->invalidate_view(view_invalid::view_redraw | view_invalid::controller);
	}

	void release_region_drag()
	{
		_state.end_region_drag();
	}

	void drag_region(const pointi from, const pointi to, const pointi element_offset)
	{
		region(df::client_rect_to_source(rectd(recti(from, to)), image_bounds(element_offset), source_extent()));
	}

	void end_region_drag(const pointi from, const pointi to, const pointi element_offset)
	{
		drag_region(from, to, element_offset);
		_state.end_region_drag();

		// A rectangle too small to see is a click that happened to move, not a region. The draw
		// replaced what was there, so too small puts it back rather than destroying it.
		const auto drawn = region_bounds(element_offset);
		if (drawn.width() < 8 || drawn.height() < 8) region(_state.region_drag_restore());
		else _host->invalidate_view(view_invalid::view_redraw | view_invalid::controller);
	}

	void cancel_region_drag()
	{
		const auto restore = _state.region_drag_restore();
		_state.end_region_drag();
		region(restore);
	}

	photo_control(view_state& state, display_state_ptr display, view_host_ptr host) :
		view_element(view_element_style::can_invoke | flex_item::media), _state(state),
		_display(std::move(display)), _host(std::move(host))
	{
		if (_display->is_one())
		{
			const auto& item = _display->_item1;
			_zoom_grading_element = std::make_shared<view_elements>();
			_zoom_grading_element->add(
				std::make_shared<rate_label_control>(_state, item, true, view_element_style::none));
			if (item->file_type()->has_trait(file_traits::edit))
			{
				_zoom_grading_element->add(
					std::make_shared<rating_control>(_state, item, true, view_element_style::none));
			}
		}
	}

	void render_zoom_thumb(ui::draw_context& dc, pointi element_offset) const;

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		if (_display->_selected_texture1)
		{
			const auto st = _display->_selected_texture1;

			_display->media_offset = element_offset;
			_display->update_for_present(dc.time_now);

			const auto generation = _display->zoom_activity_generation();
			if (generation != _zoom_quality_generation)
			{
				_zoom_quality_generation = generation;
				const auto weak = weak_from_this();
				ui::animations[&_zoom_quality_generation] = [weak]
				{
					const auto element = weak.lock();
					if (!element) return false;
					element->_host->invalidate_view(view_invalid::view_redraw);
					return element->_display->zoom_interactive(df::now());
				};
				_host->invalidate_view(view_invalid::animations);
			}
			if (const auto request = _display->panorama_draw_request(); request.active)
			{
				st->draw_panorama(dc, element_offset, request.geometry, request.view);
			}
			else
			{
				st->draw(dc, element_offset, 0, true, _display->zoom_interactive(dc.time_now));
			}

			// The zoom chrome is drawn after the region for the same reason it is hit-tested before it:
			// a rectangle drawn across the navigator must not hide it and must not claim its clicks.
			render_region(dc, element_offset);
			if (_display->zoom()) render_zoom_thumb(dc, element_offset);
			if (_display->is_zoom_mode() && !_display->is_temporary_zoom() && _zoom_grading_element &&
				!_zoom_grading_element->bounds.is_empty())
			{
				// dc is shared by the whole frame, so the alpha is put back on every exit rather than
				// on the normal one: an element that threw would otherwise dim everything painted after it.
				const auto original_alpha = dc.colors.alpha;
				const df::scope_exit restore_alpha([&dc, original_alpha] { dc.colors.alpha = original_alpha; });
				dc.colors.alpha *= std::min(dc.colors.overlay_alpha, _display->_zoom_overlay_alpha);
				const auto grading_bounds = _zoom_grading_element->bounds.offset(element_offset);
				dc.draw_rect(grading_bounds.inflate(dc.padding1), ui::color(0, dc.colors.alpha * 0.5f));
				_zoom_grading_element->render(dc, element_offset);
			}
		}
		else
		{
			// Nothing was painted, so the region's three buttons are not on screen. Their rectangles are
			// read by the hit test, and leaving the last frame's behind would route a click to a control
			// that is not there.
			_region_close_bounds.clear();
			_region_zoom_bounds.clear();
			_region_crop_bounds.clear();
		}
	}

	void render_region(ui::draw_context& dc, const pointi element_offset) const
	{
		_region_close_bounds.clear();
		_region_zoom_bounds.clear();
		_region_crop_bounds.clear();

		const auto region = region_bounds(element_offset);
		if (region.is_empty()) return;

		const auto border_clr = ui::color(ui::style::color::dialog_selected_background, dc.colors.alpha);
		dc.draw_border(region, region.inflate(df::round(2 * dc.scale_factor)), border_clr, border_clr);

		// Hidden during a drag: the buttons would sit under the pointer that is still drawing the
		// rectangle they belong to.
		if (_state.drawing_region()) return;

		const auto cxy = dc.icon_cxy;
		const auto gap = dc.padding1;
		const auto strip_width = cxy * 3 + gap * 2;

		// Outside the rectangle when it is too small to hold them, so a small selection is still
		// something a user can act on rather than something they have to redraw larger first. Either
		// way the strip is clamped into the element: placed below a region at the bottom edge it
		// would be drawn outside the clip and hit-tested where nothing routes, which is a region the
		// pointer can neither act on nor dismiss.
		const auto inside = region.width() >= strip_width + gap * 2 && region.height() >= cxy + gap * 2;
		const auto limit = bounds.offset(element_offset);
		const auto left = std::clamp(inside ? region.right - strip_width - gap : region.left,
		                             limit.left, std::max(limit.left, limit.right - strip_width));
		const auto top = std::clamp(inside ? region.top + gap : region.bottom + gap,
		                            limit.top, std::max(limit.top, limit.bottom - cxy));

		_region_close_bounds = recti(left, top, left + cxy, top + cxy);
		_region_zoom_bounds = _region_close_bounds.offset(cxy + gap, 0);
		_region_crop_bounds = _region_zoom_bounds.offset(cxy + gap, 0);

		const auto strip = recti(_region_close_bounds.left, top, _region_crop_bounds.right, top + cxy);
		dc.draw_rounded_rect(strip.inflate(gap), ui::color(0, dc.colors.alpha * 0.66f), dc.padding1);

		const auto clr = ui::color(dc.colors.foreground, dc.colors.alpha);
		xdraw_icon(dc, icon_index::close, _region_close_bounds, clr, {});
		xdraw_icon(dc, icon_index::zoom_in, _region_zoom_bounds, clr, {});
		xdraw_icon(dc, icon_index::crop, _region_crop_bounds, clr, {});
	}

	sizei calc_tex_extent(const int width_limit, const int height_limit) const
	{
		if (_display->_selected_texture1)
		{
			const auto st = _display->_selected_texture1;
			const auto dimensions = sized(st->calc_display_dimensions());
			if (dimensions.Width <= 0.0 || dimensions.Height <= 0.0)
				return {width_limit, std::min(width_limit, height_limit)};
			const auto viewport = sized(width_limit, height_limit);
			const auto fit = df::zoom_view_state::fit_scale(dimensions, viewport, setting.scale_up);
			const auto image_scale = _display->zoom_state().effective_scale(fit);

			const auto cx = std::max(1.0, dimensions.Width * image_scale);
			const auto cy = std::max(1.0, dimensions.Height * image_scale);
			return {df::round(cx), df::round(cy)};
		}

		return {};
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto tex_extent = calc_tex_extent(width_limit, width_limit * 3);
		return {width_limit, tex_extent.cy};
	}

	void layout(ui::measure_context& mc, ui::control_layouts& positions)
	{
		if (_zoom_grading_element) _zoom_grading_element->bounds.clear();
		if (_display->_selected_texture1)
		{
			const auto st = _display->_selected_texture1;
			const auto i = _display->_item1;
			const auto source = sized(st->calc_display_dimensions());
			const auto viewport = sized(bounds.extent());
			_display->zoom_layout(source, viewport, pointd(bounds.top_left()));
			const auto geometry = _display->zoom_state().geometry(source, viewport, _display->zoom_fit_scale());

			// A projection has no image rectangle: the sphere is drawn across the whole viewport, and
			// what is off screen is behind the camera rather than outside a destination rect.
			const auto projected = _display->is_panorama_projected();
			// Loading and failure feedback need a viewport before source dimensions are known.
			const auto image_bounds = projected || source.Width <= 0.0 || source.Height <= 0.0
				                          ? bounds
				                          : geometry.destination.offset(pointd(bounds.top_left())).round();

			st->layout(mc, image_bounds, i);

			const auto pan_extent = image_bounds.extent();
			_can_pan = i && i->file_type()->has_trait(file_traits::zoom) &&
				(projected || bounds.width() < pan_extent.cx || bounds.height() < pan_extent.cy);

			if (_display->is_zoom_mode() && _zoom_grading_element)
			{
				const auto grading_limit = bounds.inflate(-mc.padding2);
				const auto grading_extent = _zoom_grading_element->measure(mc, grading_limit.width());
				const recti grading_bounds{
					grading_limit.right - grading_extent.cx,
					grading_limit.bottom - grading_extent.cy,
					grading_limit.right, grading_limit.bottom
				};
				_zoom_grading_element->layout(mc, grading_bounds, positions);
			}
		}
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;
		layout(mc, positions);
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (_zoom_grading_element) _zoom_grading_element->dispatch_event(event);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;

	friend class zoom_controller;
	friend class pan_controller;
	friend class zoom_navigator_controller;
	friend class zoom_command_controller;
	friend class zoom_options_controller;
};

class video_control final : public view_element, public std::enable_shared_from_this<video_control>
{
public:
	view_state& _state;
	display_state_ptr _display;
	view_host_ptr _host;

	video_control(view_state& s, display_state_ptr display, view_host_ptr host) noexcept :
		view_element(view_element_style::can_invoke | flex_item::media), _state(s),
		_display(std::move(display)), _host(std::move(host))
	{
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		try
		{
			if (_display->_selected_texture1)
			{
				const auto st = _display->_selected_texture1;

				_display->media_offset = element_offset;
				_display->update_for_present(dc.time_now);

				if (_display->player_has_video() && !st->_vid_tex)
				{
					st->_vid_tex = dc.create_texture();
				}

				st->draw(dc, element_offset, 0, false);
			}
		}
		catch (std::exception& e)
		{
			// A frame that cannot be drawn is dropped rather than propagated out of paint. df::trace
			// is compiled out of a shipping build, so the reason has to reach the log or a blank
			// video is a defect with no evidence; log_once bounds a failure that repeats per frame.
			df::log_once(__FUNCTION__, e.what());
		}
	}

	sizei calc_tex_extent(const int width_limit, const int height_limit) const
	{
		sizei result;

		if (_display->_selected_texture1)
		{
			const auto st = _display->_selected_texture1;
			const auto ww = width_limit;
			const auto dimensions = sized(st->calc_display_dimensions());
			auto image_scale = calc_thumb_scale(dimensions, sized(ww, height_limit), false);
			if (!setting.scale_up && image_scale > 1.0) image_scale = 1.0;

			const auto cx = std::max(1.0, dimensions.Width * image_scale);
			const auto cy = std::max(1.0, dimensions.Height * image_scale);
			result = sizei(df::round(cx), df::round(cy));
		}

		return result;
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto tex_extent = calc_tex_extent(width_limit, width_limit * 3);
		return {width_limit, tex_extent.cy};
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts&) override
	{
		bounds = bounds_in;

		if (_display->_selected_texture1)
		{
			const auto st = _display->_selected_texture1;
			const auto i = _display->_item1;

			const auto layout_extent = calc_tex_extent(bounds.width(), bounds.height());
			st->layout(mc, center_rect(layout_extent, bounds), i);
		}
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			_state.play(event.host);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;

	friend class zoom_controller;
	friend class pan_controller;
};


class audio_control final : public view_element, public std::enable_shared_from_this<audio_control>
{
public:
	view_state& _state;
	view_host_ptr _host;
	display_state_ptr _display;
	// Fullscreen gives the visualizer the whole view; in the items column it is one pane above the
	// information, and an audio file is the case where the information is what the user came for.
	const bool _fills_view;

	audio_control(view_state& s, display_state_ptr display, view_host_ptr host,
	              const bool fills_view = false) noexcept :
		view_element(view_element_style::can_invoke | flex_item::media), _state(s), _host(std::move(host)),
		_display(std::move(display)), _fills_view(fills_view)
	{
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		if (_fills_view) return {width_limit, std::max(width_limit, df::round(500 * mc.scale_factor))};

		// Cover art is the only picture an audio file has, and layout gives it the top three fifths of
		// the pane, so the pane is that art scaled to the width and grown back. Without art there is no
		// picture to make room for: the visualizer takes a band and the metadata below keeps the rest.
		const auto art = _display->_selected_texture1
			                 ? _display->_selected_texture1->calc_display_dimensions()
			                 : sizei{};

		if (art.cx <= 0 || art.cy <= 0)
		{
			return {width_limit, df::round(128 * mc.scale_factor)};
		}

		const auto art_cy = std::min(df::mul_div(art.cy, width_limit, art.cx), width_limit);
		return {width_limit, std::max(df::round(128 * mc.scale_factor), df::mul_div(art_cy, 5, 3))};
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		_display->_audio_element_offset = element_offset;
		_display->_audio_element_alpha = dc.colors.alpha;

		if (_display->_selected_texture1)
		{
			const auto st = _display->_selected_texture1;

			_display->media_offset = element_offset;
			_display->update_for_present(dc.time_now);

			if (_display->player_has_video() && !st->_vid_tex)
			{
				st->_vid_tex = dc.create_texture();
			}

			if (_display->player_has_video() && st->_vid_tex)
			{
				st->draw(dc, element_offset, 0, false);
			}
		}

		if (!_display->_audio_verts)
		{
			_display->_audio_verts = dc.create_vertices();
		}

		if (_display->_audio_verts)
		{
			_display->_audio_element_bounds = bounds;
			_display->update_for_present(dc.time_now);
			dc.draw_vertices(_display->_audio_verts);
		}
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts&) override
	{
		bounds = bounds_in;

		if (bounds.width() > 100)
		{
			if (_display->_selected_texture1)
			{
				const auto st = _display->_selected_texture1;
				const auto i = _display->_item1;

				auto limit_bounds = bounds;
				limit_bounds.bottom = bounds.top + df::mul_div(bounds.height(), 3, 5);

				const auto tex_bounds = ui::scale_dimensions(st->calc_display_dimensions(), limit_bounds);
				st->layout(mc, center_rect(tex_bounds, limit_bounds), i);
			}
		}
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::invoke)
		{
			_state.play(event.host);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;

	friend class zoom_controller;
	friend class pan_controller;
};

class side_by_side_control final : public view_element, public std::enable_shared_from_this<side_by_side_control>
{
public:
	static constexpr int compare_timeline_height = 64;
	static constexpr int compare_timeline_top_padding = 12;
	view_state& _state;
	display_state_ptr _display;
	view_host_ptr _host;
	mutable recti _zoom_navigator_bounds;
	mutable recti _zoom_fit_bounds;
	mutable recti _zoom_out_bounds;
	mutable recti _zoom_100_bounds;
	mutable recti _zoom_in_bounds;
	mutable recti _zoom_options_bounds;
	// Never populated here: a projection is a judgement about one picture, so it never appears
	// while two are being compared. Carried so both controls share one overlay renderer.
	mutable recti _zoom_projection_bounds;
	std::array<view_elements_ptr, 2> _zoom_grading_elements;
	// Height reserved above the images for the A and B pane markers; zero while magnified or split.
	mutable int _pane_marker_height = 0;

	side_by_side_control(view_state& state, display_state_ptr display, view_host_ptr host) :
		view_element(flex_item::media), _state(state), _display(std::move(display)), _host(std::move(host))
	{
		const std::array items{_display->_item1, _display->_item2};
		for (auto index = 0u; index < items.size(); ++index)
		{
			const auto& item = items[index];
			auto grading = std::make_shared<view_elements>();
			grading->add(std::make_shared<rate_label_control>(_state, item, true, view_element_style::none));
			if (item->file_type()->has_trait(file_traits::edit))
			{
				grading->add(std::make_shared<rating_control>(_state, item, true, view_element_style::none));
			}
			_zoom_grading_elements[index] = std::move(grading);
		}
	}

	void render_zoom_thumb(ui::draw_context& dc, pointi element_offset) const;

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		_display->_pane_marker_bounds = {};
		const auto compare_pos = calc_compare_pos();

		if (_display->is_two())
		{
			_display->media_offset = element_offset;
			_display->update_for_present(dc.time_now);

			const auto interactive = _display->zoom_interactive(dc.time_now);
			if (_display->is_zoom_mode())
			{
				const auto texture = _display->active_zoom_pane() == df::zoom_pane::primary
					                     ? _display->_selected_texture1
					                     : _display->_selected_texture2;
				texture->draw(dc, element_offset, 0, true, interactive);
			}
			else
			{
				_display->_selected_texture1->draw(dc, element_offset, compare_pos, true, interactive);
				_display->_selected_texture2->draw(dc, element_offset, compare_pos, false, interactive);
			}
			// Both alphas are restored on every exit path, not just the normal one: dc is shared by the
			// whole frame, so an element that threw would leave everything painted afterwards dimmed.
			const auto original_overlay_alpha = dc.colors.overlay_alpha;
			const df::scope_exit restore_overlay_alpha(
				[&dc, original_overlay_alpha] { dc.colors.overlay_alpha = original_overlay_alpha; });

			if (_display->is_zoom_mode())
			{
				dc.colors.overlay_alpha = std::min(dc.colors.overlay_alpha, _display->_zoom_overlay_alpha);
			}

			if (_display->zoom()) render_zoom_thumb(dc, element_offset);
			if (_display->is_zoom_mode() && !_display->is_temporary_zoom())
			{
				const auto index = _display->active_zoom_pane() == df::zoom_pane::primary ? 0u : 1u;
				const auto& grading = _zoom_grading_elements[index];
				if (grading && !grading->bounds.is_empty())
				{
					const auto original_alpha = dc.colors.alpha;
					const df::scope_exit restore_alpha([&dc, original_alpha] { dc.colors.alpha = original_alpha; });
					dc.colors.alpha *= dc.colors.overlay_alpha;
					const auto grading_bounds = grading->bounds.offset(element_offset);
					dc.draw_rect(grading_bounds.inflate(dc.padding1), ui::color(0, dc.colors.alpha * 0.5f));
					grading->render(dc, element_offset);
				}
			}

			if (!_display->_comparing && _display->is_comparison())
			{
				const auto client = bounds.offset(element_offset);
				const auto active = _display->active_zoom_pane();
				const auto can_zoom = _display->can_zoom();

				if (_display->is_zoom_mode())
				{
					// One pane owns the canvas, so its name goes in the free corner and fades with the other overlays.
					const auto alpha = dc.colors.overlay_alpha;
					const std::string_view text = active == df::zoom_pane::primary ? "A" : "B";
					const auto extent = dc.measure_text(text, ui::style::font_face::title,
					                                    ui::style::text_style::single_line_center, 200);
					const recti letter(client.right - dc.padding2 - std::max(extent.cx, extent.cy),
					                   client.top + dc.padding2, client.right - dc.padding2,
					                   client.top + dc.padding2 + extent.cy);
					_display->_pane_marker_bounds[0] = letter.inflate(dc.padding1);
					dc.draw_rect(_display->_pane_marker_bounds[0], ui::color(0, alpha * 0.5f));
					dc.draw_text(text, letter, ui::style::font_face::title,
					             ui::style::text_style::single_line_center,
					             ui::color(dc.colors.foreground, alpha), {});
				}
				else if (_pane_marker_height > 0)
				{
					const auto alpha = dc.colors.alpha;
					const auto marker_bottom = client.top + _pane_marker_height;
					const auto underline = std::max(2, df::round(2 * dc.scale_factor));

					for (auto i = 0; i < 2; ++i)
					{
						const std::string_view text = i == 0 ? "A" : "B";
						const auto is_active = can_zoom && (i == 0) == (active == df::zoom_pane::primary);
						const recti label(client.left + df::mul_div(i, client.width(), 2), client.top,
						                  client.left + df::mul_div(i + 1, client.width(), 2), marker_bottom);
						dc.draw_text(text, label, ui::style::font_face::title,
						             ui::style::text_style::single_line_center,
						             ui::color(dc.colors.foreground, is_active || !can_zoom ? alpha : alpha * 0.5f),
						             {});

						const auto extent = dc.measure_text(text, ui::style::font_face::title,
						                                    ui::style::text_style::single_line_center, 200);
						const auto width = std::max(extent.cx, underline * 3);
						const auto center_x = label.center().x;
						const recti marker(center_x - width / 2, client.top, center_x - width / 2 + width,
						                   marker_bottom);

						if (can_zoom) _display->_pane_marker_bounds[i] = marker.inflate(dc.padding1);

						if (is_active)
						{
							// The active pane is marked under its own letter, so nothing is drawn over the image.
							dc.draw_rect(recti(marker.left, marker_bottom - underline, marker.right, marker_bottom),
							             ui::color(ui::style::color::view_selected_background, alpha));
						}
					}
				}
			}

			if (_display->_comparing)
			{
				auto split_bounds = bounds.offset(element_offset);
				split_bounds.left = std::max(split_bounds.left, compare_pos - 1);
				split_bounds.right = std::min(split_bounds.right, compare_pos + 1);
				dc.draw_rect(split_bounds, ui::color(0, dc.colors.overlay_alpha));

				const auto icon_size = std::max(dc.padding2, dc.scroll_width);
				const auto icon_top = bounds.bottom + element_offset.y - icon_size - dc.padding1;
				xdraw_icon(dc, icon_index::small_left,
				           recti(compare_pos - icon_size, icon_top, compare_pos, icon_top + icon_size),
				           ui::color(dc.colors.foreground, dc.colors.overlay_alpha), {});
				xdraw_icon(dc, icon_index::small_right,
				           recti(compare_pos, icon_top, compare_pos + icon_size, icon_top + icon_size),
				           ui::color(dc.colors.foreground, dc.colors.overlay_alpha), {});
			}
			else if (_display->_is_compare_video)
			{
				const auto scrubber = _display->_compare_video_scrubber_bounds.offset(element_offset);
				const auto is_tracking = is_style_bit_set(view_element_style::tracking);
				const auto is_hover = is_style_bit_set(view_element_style::hover);
				const auto md1 = _display->_item1->metadata();
				const auto md2 = _display->_item2->metadata();
				const auto duration1 = md1 ? std::max(1, static_cast<int>(md1->duration)) : 1;
				const auto duration2 = md2 ? std::max(1, static_cast<int>(md2->duration)) : 1;
				const auto max_duration = std::max(duration1, duration2);
				const auto row_gap = std::max(dc.padding1, 4);
				const auto row_height = std::max(4, (scrubber.height() - row_gap) / 2);
				const auto track_color = ui::color(0, dc.colors.alpha / 3.33f);
				const auto handle_color = view_handle_color(false, is_hover, is_tracking, dc.frame_has_focus, true).aa(
					dc.colors.alpha);
				const auto text_color = ui::color(dc.colors.foreground, dc.colors.alpha);
				const auto scale1 = df::round(1 * dc.scale_factor);

				for (auto row = 0; row < 2; ++row)
				{
					const auto duration = row == 0 ? duration1 : duration2;
					auto track = scrubber;
					track.top += row * (row_height + row_gap);
					track.bottom = track.top + row_height;
					track.right = track.left + df::mul_div(track.width(), duration, max_duration);

					auto track_background = track;
					if (!is_hover && !is_tracking)
					{
						track_background.top += dc.padding1;
						track_background.bottom -= dc.padding1;
					}
					dc.draw_rounded_rect(track_background, track_color, dc.padding1);

					const auto elapsed = df::mul_div(std::clamp(_display->_compare_video_pos, 0, scrubber.width()),
					                                 max_duration, std::max(1, scrubber.width()));
					const auto playhead = track.left +
						df::mul_div(std::min(elapsed, duration), track.width(), duration);
					auto progress = track;
					progress.left += scale1;
					// A much shorter clip scales its track down to a few pixels, so the high bound
					// can fall below the low one - inverted bounds are undefined for std::clamp.
					const auto progress_low = progress.left + scale1 * 2;
					progress.right = std::clamp(playhead, progress_low, std::max(progress_low, track.right - scale1));
					progress.top += scale1;
					progress.bottom -= scale1;
					dc.draw_rounded_rect(progress, handle_color, dc.padding1);

					auto duration_bounds = track;
					duration_bounds.left = track.right + dc.padding1;
					duration_bounds.right = duration_bounds.left + _display->_time_width;
					dc.draw_text(str::format_seconds(duration), duration_bounds, ui::style::font_face::dialog,
					             ui::style::text_style::single_line, text_color, {});
				}
			}
			else if (!_display->is_zoom_mode())
			{
				// Only the two-pane arrangement has a divider to grab.
				auto rr = comparison_divider_device_bounds(_display->_compare_bounds, element_offset);
				rr.left = (rr.left + rr.right) / 2;
				rr.right = rr.left + 1;
				dc.draw_rect(rr, ui::color(ui::average(dc.colors.background, 0), dc.colors.alpha));
			}
		}
	}

	std::vector<sizei> calc_normal_extents(const int width_limit, const int height_limit) const
	{
		std::vector<sizei> result;
		if (_display->is_two())
		{
			const auto ww = _display->_comparing ? width_limit : (width_limit - 10) / 2;
			const auto reserved_height = (_display->_is_compare_video ? compare_timeline_height : 0) +
				_pane_marker_height;
			const auto selected_textures = {_display->_selected_texture1, _display->_selected_texture2};

			for (const auto& st : selected_textures)
			{
				const auto dimensions = sized(st->calc_display_dimensions());
				if (dimensions.Width <= 0.0 || dimensions.Height <= 0.0)
				{
					result.emplace_back(std::max(1, ww), std::min(std::max(1, ww),
					                    std::max(1, height_limit - reserved_height)));
					continue;
				}
				auto image_scale = calc_thumb_scale(dimensions, sized(ww, std::max(1, height_limit - reserved_height)),
				                                    false);
				if (!setting.scale_up && image_scale > 1.0) image_scale = 1.0;
				const auto cx = std::max(1.0, dimensions.Width * image_scale);
				const auto cy = std::max(1.0, dimensions.Height * image_scale);
				result.emplace_back(df::round(cx), df::round(cy));
			}
		}

		return result;
	}

	// Panes are named in reserved space above the images; a magnified pane is named by an overlay instead.
	int calc_pane_marker_height(ui::measure_context& mc) const
	{
		if (!_display->is_comparison() || _display->_comparing || _display->is_zoom_mode()) return 0;
		return mc.measure_text("A", ui::style::font_face::title, ui::style::text_style::single_line_center, 200).cy +
			mc.padding2;
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		_pane_marker_height = calc_pane_marker_height(mc);

		if (_display->is_two())
		{
			const auto layout_bounds = calc_normal_extents(width_limit, width_limit);

			auto cy = 0;

			for (const auto& extent : layout_bounds)
			{
				if (cy < extent.cy) cy = extent.cy;
			}

			return {
				width_limit,
				cy + _pane_marker_height + (_display->_is_compare_video ? compare_timeline_height : 0)
			};
		}

		return {};
	}

	void layout(ui::measure_context& mc, ui::control_layouts& positions) const
	{
		_pane_marker_height = calc_pane_marker_height(mc);

		for (const auto& grading : _zoom_grading_elements)
		{
			if (grading) grading->bounds.clear();
		}
		if (_display->is_two())
		{
			constexpr auto tex_count = 2;
			const auto zoom_mode = _display->is_zoom_mode();
			const auto zoomed_pane = _display->active_zoom_pane();
			const auto layout_extent = calc_normal_extents(bounds.width(), bounds.height());
			df::assert_true(layout_extent.size() == tex_count);

			const std::array<texture_state_ptr, 2> selected_textures{
				_display->_selected_texture1, _display->_selected_texture2
			};
			const std::array<df::item_element_ptr, 2> selected_items{_display->_item1, _display->_item2};

			for (auto i = 0u; i < tex_count; i++)
			{
				const auto pane = i == 0 ? df::zoom_pane::primary : df::zoom_pane::secondary;
				if (zoom_mode && pane != zoomed_pane) continue;

				auto tex_bounds = bounds;
				tex_bounds.top += _pane_marker_height;
				if (_display->_is_compare_video) tex_bounds.bottom -= compare_timeline_height;

				if (!_display->_comparing)
				{
					if (!zoom_mode)
					{
						tex_bounds.left = bounds.left + df::mul_div(i, bounds.width(), tex_count);
						tex_bounds.right = bounds.left + df::mul_div(i + 1, bounds.width(), tex_count);

						if (i == 0) tex_bounds = tex_bounds.offset(-1, 0);
						if (i == 1) tex_bounds = tex_bounds.offset(1, 0);
					}
					const auto source = sized(selected_textures[i]->calc_display_dimensions());
					const auto viewport = sized(tex_bounds.extent());
					_display->zoom_layout(source, viewport, pointd(tex_bounds.top_left()), pane);
					const auto geometry = _display->zoom_state(pane).geometry(source, viewport,
					                                                          _display->zoom_fit_scale(pane));
					const auto image_bounds = source.Width <= 0.0 || source.Height <= 0.0
						                          ? tex_bounds
						                          : geometry.destination.offset(pointd(tex_bounds.top_left())).round();
					selected_textures[i]->layout(mc, image_bounds, selected_items[i]);
					continue;
				}

				selected_textures[i]->layout(mc, center_rect(layout_extent[i], tex_bounds), selected_items[i]);
			}

			if (_display->_is_compare_video)
			{
				const auto md1 = _display->_item1->metadata();
				const auto md2 = _display->_item2->metadata();
				const auto duration1 = md1 ? static_cast<int>(md1->duration) : 0;
				const auto duration2 = md2 ? static_cast<int>(md2->duration) : 0;
				const auto duration_text = str::format_seconds(std::max(duration1, duration2));
				_display->_time_width = mc.measure_text(duration_text, ui::style::font_face::dialog,
				                                        ui::style::text_style::single_line, 200).cx;

				_display->_compare_video_control_bounds = bounds;
				_display->_compare_video_control_bounds.top = bounds.bottom - compare_timeline_height;
				_display->_compare_video_scrubber_bounds = _display->_compare_video_control_bounds.inflate(-mc.padding2,
					-mc.padding2);
				_display->_compare_video_scrubber_bounds.top += compare_timeline_top_padding;
				_display->_compare_video_scrubber_bounds.right -= _display->_time_width + mc.padding1;
			}

			auto handle_bounds = bounds;
			handle_bounds.top += _pane_marker_height;
			_display->_compare_bounds = center_rect(sizei{mc.scroll_width, df::mul_div(handle_bounds.height(), 5, 7)},
			                                        handle_bounds);

			if (zoom_mode)
			{
				const auto index = zoomed_pane == df::zoom_pane::primary ? 0u : 1u;
				const auto& grading = _zoom_grading_elements[index];
				if (grading)
				{
					const auto grading_limit = bounds.inflate(-mc.padding2);
					const auto grading_extent = grading->measure(mc, grading_limit.width());
					const recti grading_bounds{
						grading_limit.right - grading_extent.cx,
						grading_limit.bottom - grading_extent.cy,
						grading_limit.right, grading_limit.bottom
					};
					grading->layout(mc, grading_bounds, positions);
				}
			}

			//if (_is_comparing)
			{
				_display->_compare_limits = bounds;

				for (const auto& tex : selected_textures)
				{
					const auto bounds = tex->display_bounds();
					if (_display->_compare_limits.left < bounds.left) _display->_compare_limits.left = bounds.left;
					if (_display->_compare_limits.right > bounds.right) _display->_compare_limits.right = bounds.right;
				}
			}
		}
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;
		layout(mc, positions);
	}

	void dispatch_event(const view_element_event& event) override
	{
		for (const auto& grading : _zoom_grading_elements)
		{
			if (grading) grading->dispatch_event(event);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;

	void compare(const int x, const bool tracking) const
	{
		const auto compare = tracking;

		if (_display->_comparing != compare)
		{
			_display->_comparing = compare;
			_display->stop_slideshow();
			_display->_async.invalidate_view(view_invalid::view_layout | view_invalid::tooltip);
		}

		if (_display->_compare_hover_loc != x)
		{
			_display->_compare_hover_loc = x;
			_display->_async.invalidate_view(view_invalid::view_redraw);
		}
	}

	int calc_compare_pos() const
	{
		if (!_display || !_display->_comparing) return 0;
		// is_empty() only rejects zero width, so widths of 1..7 would invert the clamp bounds.
		if (_display->_compare_limits.width() < 8)
			return (_display->_compare_limits.left + _display->_compare_limits.
			                                                   right) / 2;
		return std::clamp(_display->_compare_hover_loc, _display->_compare_limits.left + 4,
		                  _display->_compare_limits.right - 4);
	}

	friend class compare_controller;
};

class images_control2 final : public view_element, public std::enable_shared_from_this<images_control2>
{
	view_state& _state;
	display_state_ptr _display;
	recti _panel_bounds;
	recti _pin_badge_bounds;

public:
	images_control2(view_state& state, display_state_ptr display) noexcept : _state(state), _display(std::move(display))
	{
	}

	sizei measure(ui::measure_context& mc, const int cx) const override
	{
		const auto image_count = _display->_selection_item_count;
		const auto cx_control = std::min(360, df::mul_div(cx, 5, 11));

		if (image_count > 0)
		{
			return {cx, std::max(180, cx_control)};
		}

		return {cx, 0};
	}

	std::vector<sizei> surface_dims(const size_t image_count, const bool has_overflow) const
	{
		std::vector<sizei> results;
		results.reserve(image_count + (has_overflow ? 1 : 0));

		for (auto index = 0u; index < std::min(image_count, _display->_surfaces.size()); ++index)
		{
			auto dimensions = _display->_surfaces[index]->dimensions();
			if (setting.show_rotated && flips_xy(_display->_surfaces[index]->orientation()))
			{
				std::swap(dimensions.cx, dimensions.cy);
			}
			results.emplace_back(dimensions);
		}

		if (has_overflow)
		{
			results.emplace_back(results.empty() ? sizei{256, 256} : results.back());
		}

		return results;
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;
		_panel_bounds = bounds_in.inflate(-mc.padding1);
		const auto collage_bounds = _panel_bounds.inflate(-mc.padding2);
		const auto minimum_cell = std::max(1, df::round(96 * mc.scale_factor));
		const auto area_capacity = static_cast<size_t>(std::max(
			1, collage_bounds.area() / (minimum_cell * minimum_cell)));
		const auto cell_capacity = std::clamp(area_capacity, 6_z, display_state_t::max_surfaces);
		const auto visible_without_overflow = std::min(_display->_surfaces.size(), cell_capacity);
		const auto has_overflow = _display->_selection_item_count > visible_without_overflow;
		_display->_collage_image_count = std::min(_display->_surfaces.size(), cell_capacity - (has_overflow ? 1 : 0));
		// Unsigned: a selection smaller than the cells built for it would otherwise read as billions.
		_display->_selection_overflow_count = _display->_selection_item_count > _display->_collage_image_count
			                                      ? _display->_selection_item_count - _display->_collage_image_count
			                                      : 0;
		_display->_surface_bounds = ui::layout_collage(
			collage_bounds, surface_dims(_display->_collage_image_count, has_overflow));

		const auto gutter = std::max(1, df::round(2 * mc.scale_factor));
		for (auto& surface_bounds : _display->_surface_bounds)
		{
			if (surface_bounds.width() > gutter * 2 && surface_bounds.height() > gutter * 2)
			{
				surface_bounds = surface_bounds.inflate(-gutter);
			}
		}

		_pin_badge_bounds = calc_pin_badge_bounds(mc.icon_cxy);
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		dc.draw_rounded_rect(_panel_bounds.offset(element_offset),
		                     ui::color(ui::style::color::group_background, dc.colors.alpha), dc.padding1);

		// Keyed on the surface count rather than on being non-empty, so a set published after the first
		// paint is uploaded rather than half-drawn against a cache built for a shorter one.
		if (_display->_textures.size() != _display->_surfaces.size())
		{
			_display->_textures.clear();
			_display->_textures.reserve(_display->_surfaces.size());

			for (const auto& s : _display->_surfaces)
			{
				auto t = dc.create_texture();

				if (t && t->update(s) == ui::texture_update_result::failed)
				{
					t.reset();
				}
				_display->_textures.emplace_back(std::move(t));
			}
		}

		const auto texture_count = std::min(_display->_textures.size(), _display->_collage_image_count);
		for (auto i = 0u; i < std::min(texture_count, _display->_surface_bounds.size()); ++i)
		{
			const auto& texture = _display->_textures[i];
			if (!texture) continue;
			const auto orientation = _display->_surfaces[i]->orientation();
			const auto draw_bounds = _display->_surface_bounds[i].offset(element_offset);
			const auto texture_dimensions = texture->dimensions();
			const auto flip = setting.show_rotated && flips_xy(orientation);
			const auto draw_width = flip ? draw_bounds.height() : draw_bounds.width();
			const auto draw_height = flip ? draw_bounds.width() : draw_bounds.height();
			const auto texture_scale = std::max(draw_width / static_cast<double>(texture_dimensions.cx),
			                                    draw_height / static_cast<double>(texture_dimensions.cy));
			const auto source_width = draw_width / texture_scale;
			const auto source_height = draw_height / texture_scale;
			const auto source_bounds = rectd((texture_dimensions.cx - source_width) / 2,
			                                 (texture_dimensions.cy - source_height) / 2,
			                                 source_width, source_height).round();
			const auto destination = setting.show_rotated
				                         ? quadd(draw_bounds).transform(to_simple_transform(orientation))
				                         : quadd(draw_bounds);
			const auto sampler = calc_sampler(draw_bounds.extent(), source_bounds.extent(), orientation);
			dc.draw_texture(texture, destination, source_bounds, dc.colors.alpha, sampler);
		}

		if (_display->_selection_overflow_count > 0 &&
			_display->_surface_bounds.size() > _display->_collage_image_count)
		{
			const auto overflow_bounds = _display->_surface_bounds[_display->_collage_image_count].offset(
				element_offset);

			// With no cells beside it there is nothing for a "+N" to be additional to, so while the
			// thumbnails are still decoding the cell names the whole selection and becomes a remainder
			// once they arrive.
			const auto text = _display->_collage_image_count == 0
				                  ? format_plural_text(tt.title_item_count_fmt,
				                                       static_cast<int64_t>(_display->_selection_item_count))
				                  : std::format("+{}", _display->_selection_overflow_count);

			dc.draw_rect(overflow_bounds, ui::color(ui::style::color::group_background, dc.colors.alpha));
			dc.draw_text(text, overflow_bounds, ui::style::font_face::dialog,
			             ui::style::text_style::single_line_center, ui::color(dc.colors.foreground, dc.colors.alpha),
			             {});
		}

		if (!_pin_badge_bounds.is_empty())
		{
			draw_pin_badge(dc, _pin_badge_bounds.offset(element_offset), dc.colors.alpha);
		}
	}

	// The pinned item always leads the collage, so its badge sits on the first cell.
	recti calc_pin_badge_bounds(const int icon_cxy) const
	{
		if (!_state._pin_item || _display->_collage_image_count == 0 || _display->_surface_bounds.empty() ||
			_display->_collage_source_items.empty())
			return {};

		if (_display->_collage_source_items.front() != _state._pin_item) return {};

		const auto cell = _display->_surface_bounds.front();
		const auto cxy = std::min({icon_cxy * 3 / 2, cell.width(), cell.height()});
		return {cell.left, cell.top, cell.left + cxy, cell.top + cxy};
	}

	// The collage fills its texture cache only while the cache is empty, and the cache lives on the
	// display state rather than on this element, so nothing else releases it. After a device loss
	// or a fall back to the CPU renderer it held textures belonging to the device that went away
	// and the collage drew nothing for the rest of the session.
	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::free_graphics_resources)
		{
			_display->_textures.clear();
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		const auto badge = _pin_badge_bounds.offset(element_offset);

		if (!_pin_badge_bounds.is_empty() && badge.contains(loc))
		{
			return std::make_shared<unpin_badge_controller>(host, _state, badge);
		}

		return nullptr;
	}
};

class split_element final : public view_element, public std::enable_shared_from_this<split_element>
{
	const ui::const_surface_ptr _surface;
	const view_element_ptr _child;
	mutable ui::texture_ptr _tex;
	mutable sizei _surface_extent;
	mutable sizei _child_extent;
	recti _surface_bounds;
	recti _child_bounds;

public:
	split_element(ui::const_surface_ptr s, view_element_ptr child, const view_element_options& style_in) noexcept :
		view_element(style_in), _surface(std::move(s)), _child(std::move(child))
	{
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		if (is_valid(_surface))
		{
			if (!_tex)
			{
				const auto t = dc.create_texture();

				if (t && t->update(_surface) != ui::texture_update_result::failed)
				{
					_tex = t;
				}
			}

			if (_tex)
			{
				dc.draw_texture(_tex, _surface_bounds.offset(element_offset));
			}
		}

		if (_child)
		{
			_child->render(dc, element_offset);
		}
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;

		const auto surface_cy_extra = (bounds_in.height() - _surface_extent.cy) / 2;
		const auto child_cy_extra = (bounds_in.height() - _child_extent.cy) / 2;

		_surface_bounds = recti({bounds_in.left, bounds_in.top + surface_cy_extra}, _surface_extent);
		_child_bounds = recti({bounds_in.left + _surface_extent.cx, bounds_in.top + child_cy_extra}, _child_extent);
		_child->layout(mc, _child_bounds, positions);
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		_child->tooltip(hover, loc, element_offset);
	}

	void hover(interaction_context& ic) override
	{
		_child->hover(ic);
	}

	void dispatch_event(const view_element_event& event) override
	{
		_child->dispatch_event(event);

		if (event.type == view_element_event_type::free_graphics_resources)
		{
			_tex.reset();
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset,
	                                             hit_test_context& ctx) override
	{
		return _child->controller_from_location(host, loc, element_offset, ctx);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		_surface_extent = ui::scale_dimensions(_surface->dimensions(), width_limit / 2, true);
		_child_extent = _child->measure(mc, width_limit - _surface_extent.cx);
		return {_child_extent.cx + _surface_extent.cx, std::max(_surface_extent.cy, _child_extent.cy)};
	}
};


class bullet_element final : public view_element, public std::enable_shared_from_this<bullet_element>
{
	const int _icon_padding = 16;
	const icon_index _icon;
	const view_element_ptr _child;
	mutable sizei _child_extent;
	recti _child_bounds;

public:
	bullet_element(const icon_index i, view_element_ptr child,
	               const view_element_options& style_in = {}) noexcept : view_element(style_in),
	                                                                     _icon(i), _child(std::move(child))
	{
	}

	bullet_element(const icon_index i, const std::string_view text) : _icon(i),
	                                                                  _child(std::make_shared<text_element>(text))
	{
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		auto icon_bounds = bounds.offset(element_offset);
		icon_bounds.right = icon_bounds.left + dc.icon_cxy + _icon_padding;
		icon_bounds.left += 8;

		xdraw_icon(dc, _icon, icon_bounds, ui::color(dc.colors.foreground, dc.colors.alpha), {});

		if (_child)
		{
			_child->render(dc, element_offset);
		}
	}


	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		bounds = bounds_in;

		const auto child_cy_extra = (bounds_in.height() - _child_extent.cy) / 2;

		_child_bounds = recti({bounds_in.left + mc.icon_cxy + _icon_padding, bounds_in.top + child_cy_extra},
		                      _child_extent);
		_child->layout(mc, _child_bounds, positions);
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		_child->tooltip(hover, loc, element_offset);
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

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto w = mc.icon_cxy + _icon_padding;
		_child_extent = _child->measure(mc, width_limit - w);
		return {_child_extent.cx + w, std::max(mc.icon_cxy, _child_extent.cy)};
	}
};
