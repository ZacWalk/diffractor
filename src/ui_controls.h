// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Custom UI controls and visual elements. Implements command links, rating controls,
// photo viewers, video controls, scrubbers, and other interactive UI components. The panel's document
// and hex dumps are ui_document.h.

#pragma once

#include "app_commands.h"
#include "ui_controllers.h"
#include "ui_elements.h"
#include "model_display.h"
#include "libdesktop/ui/table_element.h"

class display_state_t;

inline recti comparison_divider_device_bounds(const recti logical_bounds, const pointi element_offset)
{
	return logical_bounds.offset(element_offset);
}

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

	explicit command_link_element(ui::command_ptr command, const view_element_options& style_in = {}) :
		view_element(style_in), _command(std::move(command))
	{
		padding = {4, 4};
		sync_command_state();
	}

	const ui::command_ptr& command() const { return _command; }

	sizei measure(render::measure_context& mc, const int width_limit) const override
	{
		if (!_command->visible) return {};

		const auto cy = mc.text_line_height(render::font_face::dialog);
		auto cx = 0;

		if (_command->icon != icon_index::none) cx += cy;

		const auto text = label();

		if (!text.empty())
		{
			cx += mc.padding1 +
			      mc.measure_text(text, render::font_face::dialog, render::text_style::single_line, width_limit).cx;
		}

		return {std::min(std::max(cx, 1), width_limit), cy};
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		if (!_command->visible) return;

		const auto logical_bounds = bounds.offset(element_offset);
		_device_bounds = logical_bounds;

		render_background(dc, element_offset);

		const auto alpha = (_command->enable ? 1.0f : 0.35f) * dc.colors.alpha;
		const auto clr = core::color(link_foreground_color(), alpha);
		const auto text = label();
		auto text_bounds = logical_bounds;

		if (progress)
		{
			const auto pos = progress();

			if (pos >= 0)
			{
				auto track = logical_bounds;
				track.top = track.bottom - core::round(3 * dc.scale_factor);
				dc.draw_rect(track, core::color(ui::style::colors.group_background, alpha * 0.7f));
				track.right = track.left + core::mul_div(std::min(pos, 1000), track.width(), 1000);
				dc.draw_rect(track, core::color(ui::style::colors.view_selected_background, alpha));
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
			dc.draw_text(text, text_bounds, render::font_face::dialog,
			             _command->icon != icon_index::none ? render::text_style::single_line
			                                                : render::text_style::single_line_center,
			             clr, {});
		}
	}

	void tooltip(view_hover_element& hover, const pointi /*loc*/, const pointi element_offset) const override
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
				event.host->invalidate(relayout ? ui::host_invalid::layout : ui::host_invalid::redraw);
			}
		}
		else if (event.type == view_element_event_type::invoke)
		{
			invoke_command(event.host);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
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

inline std::string_view orientation_to_string(const core::orientation& o) noexcept
{
	switch (o)
	{
	case core::orientation::top_left: return tt.orientation_top_left;
	case core::orientation::top_right: return tt.orientation_top_right;
	case core::orientation::bottom_right: return tt.orientation_bottom_right;
	case core::orientation::bottom_left: return tt.orientation_bottom_left;
	case core::orientation::left_top: return tt.orientation_left_top;
	case core::orientation::right_top: return tt.orientation_right_top;
	case core::orientation::right_bottom: return tt.orientation_right_bottom;
	case core::orientation::left_bottom: return tt.orientation_left_bottom;
	default: break;
	}

	return {};
}

namespace ui
{
	std::vector<recti> layout_collage(recti draw_bounds, const std::vector<sizei>& dimensions);
}

class rating_control final :
	public std::enable_shared_from_this<rating_control>,
	public view_element,
	public keyboard_parts_source
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
	               const view_element_options& style_in) noexcept :
		view_element(style_in), _state(s), show_accelerator(show_accelerator), _item(std::move(i)), _can_edit([this]
	{
		const df::item_set items = {{_item}};
		return items.can_process(df::process_items_type::can_save_metadata, false, {}).success();
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

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto bg = calc_background_color(dc);
		const auto is_hover = is_style_bit_set(view_element_style::hover);
		const auto rating = is_hover ? last_hover_rating : displayed_rating();
		const auto clr = core::color(dc.colors.foreground, _can_edit ? dc.colors.alpha : dc.colors.alpha / 4.0f);

		std::string text;

		for (auto i = 0; i < 5; i++)
		{
			text += icon_to_utf8(i < rating ? icon_index::star_solid : icon_index::star);
		}

		dc.draw_text(text, logical_bounds, render::font_face::icons, render::text_style::single_line_center, clr, bg);
	}

	// The bubble states both grading values, because Reject and the stars share one metadata field
	// and a user who cannot see that will not understand why a star cleared their Reject mark.
	void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;

	sizei measure(render::measure_context& mc, const int /*width_limit*/) const override
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
			ic.redraw = true;
		}

		last_hover_rating = _hover_rating;
	}

	static int to_rating(const recti rating_bounds, const pointi loc)
	{
		// Clamped: the cells are whole divisions of the width, so the pixels past the last full cell
		// - and the inclusive right edge - divided out to a sixth star no rating has.
		return std::clamp((loc.x - rating_bounds.left) / std::max(1, rating_bounds.width() / 5) + 1, 1, 5);
	}

	// A star each, where to_rating finds it.
	std::vector<recti> keyboard_parts() const override
	{
		if (!can_invoke()) return {};

		std::vector<recti> parts;
		parts.reserve(5);
		const auto cell = std::max(1, bounds.width() / 5);
		for (auto i = 0; i < 5; ++i)
			parts.emplace_back(bounds.left + i * cell, bounds.top, bounds.left + (i + 1) * cell, bounds.bottom);
		return parts;
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	void dispatch_event(const view_element_event& event) override;
};

// The grading picker for a pending value rather than an item. The batch metadata surface edits a
// number that has not been written anywhere yet, so it cannot reuse rating_control, but the user
// must still see the same vocabulary - Reject plus five stars - that grading shows everywhere else.
class rating_edit_control final :
	public std::enable_shared_from_this<rating_edit_control>,
	public view_element,
	public keyboard_parts_source
{
	int& _val;
	std::function<void(int)> _changed;
	mutable int _icon_cxy = 0;
	int _hover_cell = -1;

public:
	// Reject occupies the first cell because it is a rating of -1, not a separate field.
	static constexpr int cell_count = 6;

	rating_edit_control(int& val, std::function<void(int)> changed) noexcept :
		view_element(view_element_style::can_invoke | view_element_style::has_tooltip), _val(val),
		_changed(std::move(changed))
	{
	}

	static int cell_to_rating(const int cell) { return cell <= 0 ? -1 : cell; }

	// Reject, then a star each, where hover finds them.
	std::vector<recti> keyboard_parts() const override
	{
		std::vector<recti> parts;
		parts.reserve(cell_count);
		const auto cxy = std::max(1, _icon_cxy);
		for (auto i = 0; i < cell_count; ++i)
			parts.emplace_back(bounds.left + i * cxy, bounds.top, bounds.left + (i + 1) * cxy, bounds.bottom);
		return parts;
	}

	sizei measure(render::measure_context& mc, const int /*width_limit*/) const override
	{
		_icon_cxy = mc.icon_cxy;
		return {mc.icon_cxy * cell_count, mc.icon_cxy};
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto bg = calc_background_color(dc);
		const auto cxy = std::max(1, _icon_cxy);
		const auto rating = _hover_cell < 0 ? _val : cell_to_rating(_hover_cell);

		auto reject_bounds = logical_bounds;
		reject_bounds.right = reject_bounds.left + cxy;

		const auto reject_text = icon_to_utf8(rate_label_reject.icon);
		const auto reject_clr = rating < 0 ? core::color(rate_label_reject.clr, dc.colors.alpha)
		                                   : core::color(dc.colors.foreground, dc.colors.alpha / 3.0f);

		dc.draw_text(reject_text, reject_bounds, render::font_face::icons, render::text_style::single_line_center,
		             reject_clr, bg);

		std::string stars;

		for (auto i = 0; i < 5; i++)
		{
			stars += icon_to_utf8(i < rating ? icon_index::star_solid : icon_index::star);
		}

		auto star_bounds = logical_bounds;
		star_bounds.left = logical_bounds.left + cxy;

		dc.draw_text(stars, star_bounds, render::font_face::icons, render::text_style::single_line_center,
		             core::color(dc.colors.foreground, dc.colors.alpha), bg);
	}

	void hover(interaction_context& ic) override
	{
		const auto logical_bounds = bounds.offset(ic.element_offset);
		const auto cell = logical_bounds.contains(ic.loc)
		                      ? std::clamp((ic.loc.x - logical_bounds.left) / std::max(1, _icon_cxy), 0, cell_count - 1)
		                      : -1;

		if (_hover_cell != cell)
		{
			_hover_cell = cell;
			ic.redraw = true;
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
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
	                   const view_element_options& style_in) noexcept :
		view_element(style_in | view_element_style::has_tooltip | view_element_style::can_invoke), _state(s),
		_item(std::move(i)), show_accelerator(show_accelerator), _can_edit([this]
	{
		const df::item_set items = {{_item}};
		return items.can_process(df::process_items_type::can_save_metadata, false, {}).success();
	}())
	{
	}

	sizei measure(render::measure_context& mc, const int /*width_limit*/) const override
	{
		return {mc.icon_cxy, mc.icon_cxy};
	}

	void render(render::draw_context& dc, pointi element_offset) const override;
	void dispatch_event(const view_element_event& event) override;
	void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
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
	                const view_element_options& style_in) noexcept :
		view_element(style_in), _state(s), _ts(std::move(ts)), show_accelerator(show_accelerator)
	{
		style |= view_element_style::has_tooltip | view_element_style::can_invoke;
	}

	sizei measure(render::measure_context& mc, const int /*width_limit*/) const override
	{
		return {mc.icon_cxy, mc.icon_cxy};
	}

	void render(render::draw_context& dc, pointi element_offset) const override;
	void dispatch_event(const view_element_event& event) override;
	void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

class items_dates_control final : public std::enable_shared_from_this<items_dates_control>, public view_element
{
	view_state& _state;
	const df::item_element_ptr _item;
	std::string _text;
	core::date_t _shown_date;
	df::date_parts_prop _shown_target = df::date_parts_prop::original;
	bool _invalid = false;
	render::font_face _font = render::font_face::dialog;
	render::text_style _text_style = render::text_style::multiline;

public:
	items_dates_control(view_state& s, df::item_element_ptr i) noexcept :
		view_element(view_element_style::has_tooltip | view_element_style::can_invoke), _state(s), _item(std::move(i))
	{
		const auto& search = s.search();

		// Each candidate is matched against the key that names it. The tile can show the
		// capture-first ladder, the Created concept or the file stamp, and a match on one of those
		// is not a match on another. The stamps are UTC instants in the index, so they are converted
		// before being compared or shown, the way the index matcher converts them.
		const auto created_concept = _item->file_or_metadata_created();
		const auto modified_date = platform::utc_to_local(_item->file_modified());

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

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		const auto bg = calc_background_color(dc);
		const auto logical_bounds = bounds.offset(element_offset);
		const auto text = _invalid ? tt.invalid.sv() : std::string_view(_text);
		dc.draw_text(text, logical_bounds, _font, _text_style, core::color(dc.colors.foreground, dc.colors.alpha), bg);
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
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
	                                             const pointi element_offset, hit_test_context& ctx) override
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
		view_element(style_in | view_element_style::has_tooltip | view_element_style::can_invoke), _state(s),
		_item(std::move(i)), show_accelerator(show_accelerator)
	{
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
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
				bg.merge(core::color(ui::style::colors.important_background, dc.colors.alpha));
			}

			xdraw_icon(dc, icon, logical_bounds, core::color(dc.colors.foreground, alpha), bg);
		}
	}

	sizei measure(render::measure_context& mc, const int /*width_limit*/) const override
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

	void tooltip(view_hover_element& hover, const pointi /*loc*/, const pointi element_offset) const override
	{
		hover.id = commands::pin_item;
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

// Surfaces that draw the pin badge but have no room for a pin_control still have to let the user
// release the item, so the badge itself is the affordance. Only a whole click on it does: the release
// after a press that closed a menu does nothing.
class unpin_badge_controller final : public view_controller
{
	view_state& _state;
	bool _pressed = false;

public:
	unpin_badge_controller(const view_host_ptr& host, view_state& state, const recti bounds) :
		view_controller(host, bounds), _state(state)
	{
	}

	platform::cursor cursor() const override { return platform::cursor::link; }

	void on_mouse_left_button_down(const pointi /*loc*/, const platform::key_state /*keys*/) override
	{
		_pressed = true;
	}

	void on_mouse_left_button_up(const pointi loc, const platform::key_state /*keys*/) override
	{
		if (std::exchange(_pressed, false) && _bounds.contains(loc))
		{
			_state._pin_item.reset();
			_state.invalidate_view(view_invalid::view_layout | view_invalid::view_redraw | view_invalid::command_state);
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
template <typename T> class item_icon_element : public view_element
{
protected:
	view_state& _state;
	const df::item_element_ptr _item;
	const icon_index _icon;

	item_icon_element(view_state& s, df::item_element_ptr i, const icon_index icon,
	                  const view_element_options& style_in) noexcept :
		view_element(style_in | view_element_style::has_tooltip | view_element_style::can_invoke), _state(s),
		_item(std::move(i)), _icon(icon)
	{
	}

public:
	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		const auto bg = calc_background_color(dc);
		xdraw_icon(dc, _icon, bounds.offset(element_offset), core::color(dc.colors.foreground, dc.colors.alpha), bg);
	}

	sizei measure(render::measure_context& mc, const int /*width_limit*/) const override
	{
		return {mc.icon_cxy, mc.icon_cxy};
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		return default_controller_from_location(static_cast<T&>(*this), host, loc, element_offset, ctx);
	}
};

class unselect_element final :
	public std::enable_shared_from_this<unselect_element>,
	public item_icon_element<unselect_element>
{
public:
	unselect_element(view_state& s, df::item_element_ptr i, const view_element_options& style_in) noexcept :
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

	void tooltip(view_hover_element& hover, const pointi /*loc*/, const pointi element_offset) const override
	{
		hover.elements->add(make_icon_element(_icon, flex_item::no_break));
		hover.elements->add(std::make_shared<text_element>(str_format(tt.unselect_fmt.sv(), _item->name())));
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}
};

class delete_element final :
	public std::enable_shared_from_this<delete_element>,
	public item_icon_element<delete_element>
{
public:
	delete_element(view_state& s, df::item_element_ptr i, const view_element_options& style_in) noexcept :
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

	void tooltip(view_hover_element& hover, const pointi /*loc*/, const pointi element_offset) const override
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
	render::font_face _font = render::font_face::dialog;
	render::text_style _text_style = render::text_style::multiline;
	std::string _text;
	icon_index _icon = icon_index::none;

public:
	stream_element(view_state& state, df::item_element_ptr i, av_stream_info stream,
	               const int audio_track_number) noexcept :
		_state(state), _item(std::move(i)), _stream(std::move(stream))
	{
		if (_stream.type == av_stream_type::audio)
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

	// What the stream is called in its row, which is also what its row sorts by under the Title heading.
	const std::string& text() const { return _text; }

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		const auto bg = calc_background_color(dc);
		auto text_bounds = logical_bounds;

		if (_icon != icon_index::none)
		{
			auto icon_bounds = logical_bounds;
			icon_bounds.right = icon_bounds.left + logical_bounds.height();
			xdraw_icon(dc, _icon, icon_bounds, core::color(link_foreground_color(), dc.colors.alpha), bg);
			text_bounds.left = icon_bounds.right + dc.padding1;
		}

		dc.draw_text(_text, text_bounds, _font, _text_style,
		             core::color(_icon == icon_index::none ? dc.colors.foreground : link_foreground_color(),
		                         dc.colors.alpha),
		             bg);
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
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

	void tooltip(view_hover_element& hover, const pointi /*loc*/, const pointi element_offset) const override
	{
		hover.elements->add(make_icon_element(icon_index::audio, flex_item::no_break));
		hover.elements->add(std::make_shared<text_element>(_text));
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}
};

inline std::shared_ptr<text_element> listing_text_cell(const std::string_view text, const bool right_align = false,
                                                       const core::color32 color = 0)
{
	auto result = std::make_shared<text_element>(text, render::font_face::dialog,
	                                             right_align ? render::text_style::single_line_far
	                                                         : render::text_style::single_line,
	                                             view_element_options{});
	if (color) result->foreground_color(color);
	return result;
}

class listing_icon_cell_element final : public ui::elements
{
	static constexpr int icon_width = 24;
	view_element_ptr _image;
	std::shared_ptr<text_element> _label;

public:
	listing_icon_cell_element(const icon_index icon, const std::string_view text, const bool right_align,
	                          const core::color32 color) :
		_image(make_icon_element(icon, view_element_options{})), _label(listing_text_cell(text, right_align, color))
	{
		flex_container.wrap = flex_wrap::no_wrap;
		_image->flex.basis = _image->flex.max_size.cx = icon_width;
		_image->flex.shrink = 1.0f;
		_label->flex.grow = 1.0f;
		_label->flex.shrink = 1.0f;
		add(_image);
		add(_label);
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
	{
		// The label grows when placed, not when the table asks for its natural column width.
		const auto image_porch = _image->porch() * mc.scale_factor;
		const auto label_porch = _label->porch() * mc.scale_factor;
		const auto image_width = core::round(icon_width * mc.scale_factor);
		const auto fixed_width = image_width + (image_porch.cx + label_porch.cx) * 2;
		const auto label = _label->measure(mc, std::max(0, width_limit - fixed_width));
		const auto image = _image->measure(mc, image_width);
		return {std::min(std::max(0, width_limit), fixed_width + label.cx),
		        std::max(image.cy + image_porch.cy * 2, label.cy + label_porch.cy * 2)};
	}
};

inline view_element_ptr listing_icon_cell(const icon_index icon, const std::string_view text,
                                          const bool right_align = false, const core::color32 color = 0)
{
	return std::make_shared<listing_icon_cell_element>(icon, text, right_align, color);
}

// A heading over a table of the stack: text names the column; numbers - sizes, dates, counts - sit at its right
// and sort largest or latest first, as they are usually wanted.
inline ui::table_heading stack_heading(const std::string_view title, const bool numeric = false)
{
	return {std::string(title), numeric, true, !numeric};
}

// The stack is rebuilt beneath a selection for many reasons - details arriving, a rescan, a session closing - and
// each rebuild makes its tables again. What the user made of one, its widths and its sort, is kept on the display by
// the table's name, so the replacement carries on from it. Call once the headings are set.
inline void keep_table_choices(const std::shared_ptr<ui::table_element>& table, const display_state_ptr& display,
                               const std::string_view name)
{
	if (!table || !display) return;

	if (const auto found = display->_table_choices.find(name); found != display->_table_choices.end())
	{
		table->restore_column_sizes(found->second.sizes);
		table->sort(found->second.sort_column, found->second.ascending);
	}

	table->on_user_change = [weak = std::weak_ptr(display), key = std::string(name)](const ui::table_element& t)
	{
		if (const auto d = weak.lock())
			d->_table_choices[key] = {t.user_column_sizes(), t.sort_column(), t.sort_ascending()};
	};
}

class summary_control final : public ui::elements
{
	const df::file_group_histogram _summary;
	std::shared_ptr<ui::table_element> _table = std::make_shared<ui::table_element>();

public:
	summary_control(const df::file_group_histogram& summary, const view_element_options& style_in) noexcept :
		ui::elements(style_in), _summary(summary)
	{
		flex_container.direction = flex_direction::column;
		flex_container.wrap = flex_wrap::no_wrap;
		flex_container.align_items = flex_align::stretch;
		flex_container.justify = flex_justify::end;
		add(_table);
		populate_lines();
	}

	void populate_lines()
	{
		_table->clear();
		for (auto i = 0; i < file_group::max_count; ++i)
		{
			const auto& c = _summary.counts[i];

			if (c.count > 0)
			{
				const auto* const ft = file_group_from_index(i);
				const auto size = c.size.is_empty() ? std::string{} : platform::format_size(c.size);
				const auto text = ft->display_name(c.count > 1);
				const auto num = platform::format_number(core::to_string(c.count));

				_table->add(listing_icon_cell(ft->icon, num, true),
				            listing_text_cell(text, false, ft->text_color(ui::style::colors.view_text)),
				            listing_text_cell(size, true));
			}
		}
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::populate)
		{
			populate_lines();
		}
		ui::elements::dispatch_event(event);
	}
};

// An archive's listing, in a panel of the media stack, under headings that sort and size its columns. The packed
// size is a column only where the archive's reader knew it.
class file_list_control final : public ui::panel_element
{
	display_state_ptr _display;
	std::shared_ptr<ui::table_element> _table = std::make_shared<ui::table_element>();

	static std::vector<ui::table_heading> headings(const bool packed)
	{
		std::vector result = {stack_heading(tt.sort_by_name.sv()), stack_heading(tt.prop_name_modified.sv(), true),
		                      stack_heading(tt.prop_name_size.sv(), true)};
		if (packed) result.emplace_back(stack_heading(tt.table_compressed.sv(), true));
		return result;
	}

public:
	file_list_control(display_state_ptr display, const view_element_options& style_in) noexcept :
		ui::panel_element(style_in), _display(std::move(display))
	{
		flex_container.direction = flex_direction::column;
		flex_container.wrap = flex_wrap::no_wrap;
		flex_container.align_items = flex_align::stretch;
		add(_table);
		populate();
	}

	void populate()
	{
		_table->clear();

		const auto& items = _display->_archive_items;
		const auto packed = std::ranges::any_of(items,
		                                        [](const archive_item& i) { return !i.compressed_size.is_empty(); });

		// The columns first, then what the user made of them: a sort by the packed size can only be kept once there
		// is a column for it.
		if (_table->headings().size() != (packed ? 4u : 3u))
		{
			_table->headings(headings(packed));
			keep_table_choices(_table, _display, "archive");
		}

		const auto size_key = [](const core::file_size size)
		{
			return size.is_empty() ? ui::table_sort_key{} : ui::table_sort_key(size.to_int64());
		};

		for (const auto& i : items)
		{
			const auto is_empty = i.uncompressed_size.is_empty();
			const auto* const ft = is_empty ? file_type::folder : files::file_type_from_name(i.filename);
			const auto color = ft->text_color(ui::style::colors.view_text);
			const auto format_size = [is_empty](const core::file_size size)
			{
				return is_empty || size.is_empty() ? std::string{} : platform::format_size(size);
			};

			std::vector<view_element_ptr> cells = {listing_icon_cell(ft->icon, i.filename, false, color),
			                                       listing_text_cell(prop::format_date(i.modified)),
			                                       listing_text_cell(format_size(i.uncompressed_size), true)};
			std::vector<ui::table_sort_key> keys = {ui::table_sort_key(i.filename),
			                                        i.modified.is_valid() ? ui::table_sort_key(i.modified.to_int64())
			                                                              : ui::table_sort_key{},
			                                        is_empty ? ui::table_sort_key{} : size_key(i.uncompressed_size)};

			if (packed)
			{
				cells.emplace_back(listing_text_cell(format_size(i.compressed_size), true));
				keys.emplace_back(is_empty ? ui::table_sort_key{} : size_key(i.compressed_size));
			}

			_table->add_row(cells, std::move(keys));
		}
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::populate)
		{
			populate();
		}
		ui::elements::dispatch_event(event);
	}
};

class play_control final : public std::enable_shared_from_this<play_control>, public view_element
{
public:
	view_state& _state;
	display_state_ptr _display;

	play_control(view_state& s, const view_element_options& style_in) noexcept :
		view_element(style_in | view_element_style::has_tooltip | view_element_style::can_invoke), _state(s),
		_display(s.display_state())
	{
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);

		render_background(dc, element_offset);

		const auto icon = _display->is_playing_media() ? icon_index::pause : icon_index::play;
		const auto clr = core::color(dc.colors.foreground, dc.colors.alpha);

		xdraw_icon(dc, icon, logical_bounds, clr, {});
	}

	sizei measure(render::measure_context& mc, const int /*width_limit*/) const override
	{
		// Transport leads the row, so it is drawn larger than the icon controls beside it - but it is
		// the same glyph font, so an unscaled box would clip it once the display scale passes 175%.
		const auto cxy = core::round(32 * mc.scale_factor);
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
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	void tooltip(view_hover_element& hover, const pointi /*loc*/, const pointi element_offset) const override
	{
		hover.id = commands::play;
		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}
};

// Seeks to where it is clicked, which the keyboard has no centre for.
class scrubber_element final :
	public std::enable_shared_from_this<scrubber_element>,
	public view_element,
	public no_keyboard_parts
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
		_display(std::move(display)), _player(std::move(player))
	{
	}

	// The track excludes the elapsed/duration labels. Derived from layout, not from render, so hit
	// testing does not depend on a paint having happened or on which view painted last.
	recti calc_track_bounds(const pointi element_offset) const
	{
		const auto logical_bounds = bounds.offset(element_offset);
		if (logical_bounds.width() <= 0 || logical_bounds.height() <= 0) return {};
		if (logical_bounds.width() <= _display->_time_width * 3)
			return {logical_bounds.left, logical_bounds.top, logical_bounds.right,
			        logical_bounds.top + logical_bounds.height() / 2};
		return logical_bounds.inflate(-_display->_time_width, 0);
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		const auto logical_bounds = bounds.offset(element_offset);
		render_background(dc, element_offset);

		const auto track_bounds = calc_track_bounds(element_offset);
		_display->_scrubber_width = track_bounds.width();

		if (!track_bounds.is_empty())
		{
			const auto is_tracking = is_style_bit_set(view_element_style::tracking);
			const auto is_hover = is_style_bit_set(view_element_style::hover);
			const auto scrub_bg_clr = core::color(0, dc.colors.alpha / 3.33f);
			const auto scale1 = core::round(1 * dc.scale_factor);

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
			const auto pos = core::round(media_pos * max_scrubber_width / media_len);

			scrub_bounds.left += scale1;
			scrub_bounds.right = scrub_bounds.left + std::clamp(pos, scale1 * 2, max_scrubber_width);
			scrub_bounds.top += scale1;
			scrub_bounds.bottom -= scale1;

			dc.draw_rounded_rect(scrub_bounds,
			                     view_handle_color(false, is_hover, is_tracking, dc.frame_has_focus, true)
			                         .aa(dc.colors.alpha),
			                     dc.padding1);

			auto time_bounds = logical_bounds;
			auto duration_bounds = logical_bounds;
			if (logical_bounds.width() <= _display->_time_width * 3)
			{
				time_bounds.top = duration_bounds.top = track_bounds.bottom;
				time_bounds.right = duration_bounds.left = logical_bounds.center().x;
			}
			else
			{
				time_bounds.right = track_bounds.left;
				duration_bounds.left = track_bounds.right;
			}

			const auto clr = core::color(dc.colors.foreground, dc.colors.alpha);

			dc.draw_text(_display->_time, time_bounds, render::font_face::dialog,
			             render::text_style::single_line_center, clr, {});
			dc.draw_text(_display->_duration, duration_bounds, render::font_face::dialog,
			             render::text_style::single_line_center, clr, {});
		}
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
	{
		const auto extent = mc.measure_text("00:00:00", render::font_face::dialog, render::text_style::single_line,
		                                    200);
		_display->_time_width = extent.cx + mc.padding2;
		// Natural width is the narrowest usable track; flex grow claims the row's spare width.
		const auto natural_width = std::min(width_limit, _display->_time_width * 4);
		return {natural_width,
		        std::max(extent.cy * (width_limit <= _display->_time_width * 3 ? 2 : 1), mc.scroll_width)};
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
				ic.redraw = true;

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

	void dispatch_event(const view_element_event& /*event*/) override {}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		const auto& surface = _display->_hover_surface;
		const auto track_bounds = calc_track_bounds(element_offset);

		if (!track_bounds.is_empty() && is_valid(surface))
		{
			const auto x = std::clamp(loc.x, track_bounds.left, track_bounds.right);

			hover.elements->add(std::make_shared<surface_element>(surface, 200, view_element_style::none));
			hover.elements->add(std::make_shared<text_element>(core::format_seconds(core::round(surface->time())),
			                                                   render::font_face::dialog,
			                                                   render::text_style::single_line,
			                                                   flex_item::center | flex_item::new_line));

			hover.window_bounds = track_bounds;
			hover.active_bounds = recti(x, track_bounds.top, x + 1, track_bounds.bottom);
			hover.x_focus = _display->_hover_scrubber_pos;
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
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

		title_element(view_element_ptr vv) : v(std::move(vv)) {}
	};

	std::vector<title_element> elements;

	group_title_control() noexcept = default;

	group_title_control(const std::string_view title, const std::vector<view_element_ptr>& other_controls = {}) noexcept
	{
		elements.emplace_back(std::make_shared<text_element>(title, render::font_face::title,
		                                                     render::text_style::multiline, flex_item::grow));
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

	void render(render::draw_context& dc, const pointi element_offset) const override
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

	sizei measure(render::measure_context& mc, const int width_limit) const override
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
		row.gap.cx = core::round(mc.padding1 / mc.scale_factor);
		return {width_limit, calc_flex_layout(children(), mc, {width_limit, -1}, row).extent.cy};
	}

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		bounds = bounds_in;

		flex_container_layout row;
		row.wrap = flex_wrap::no_wrap;
		row.gap.cx = core::round(mc.padding1 / mc.scale_factor);
		const auto calculated = calc_flex_layout(children(), mc, bounds.extent(), row);
		for (auto i = 0u; i < elements.size(); ++i)
		{
			const auto& element = elements[i];
			const auto child_bounds = calculated.layout_bounds[i].offset(bounds.top_left());
			element.visible = child_bounds.right <= bounds.right;
			if (element.visible) element.v->layout(mc, child_bounds);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
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

	// What the title holds as it draws them: those its row had room for.
	void visit_children(const std::function<void(const view_element_ptr&)>& visit) const override
	{
		for (const auto& e : elements)
		{
			if (e.visible) visit(e.v);
		}
	}

private:
	mutable std::vector<view_element_ptr> _children;
};

// The keyboard zooms the picture with keys of its own, so the picture itself is no part of it.
class photo_control final :
	public view_element,
	public std::enable_shared_from_this<photo_control>,
	public no_keyboard_parts
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

	core::file_path region_item() const
	{
		// _display is non-null for this control's whole lifetime - the constructor reads it - so the
		// question here is only whether it resolves to one picture.
		return _display->is_one() && _display->_item1 ? _display->_item1->path() : core::file_path{};
	}

	rectd region() const { return _state.drawn_region(region_item()); }

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
		_host->invalidate(ui::host_invalid::redraw | ui::host_invalid::controller);
	}

	void clear_region()
	{
		if (!has_region()) return;
		_state.drawn_region(region_item(), {});
		_state.end_region_drag();
		_host->invalidate(ui::host_invalid::redraw | ui::host_invalid::controller);
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
		_host->invalidate(ui::host_invalid::redraw | ui::host_invalid::controller);
	}

	// Moving keeps the rectangle; only the buttons go, because the pointer is over them. They are
	// painted, so their leaving is a redraw the drag itself has to ask for.
	void begin_region_move()
	{
		_state.begin_region_drag(region_item(), false);
		_host->invalidate(ui::host_invalid::redraw | ui::host_invalid::controller);
	}

	void release_region_drag() { _state.end_region_drag(); }

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
		else _host->invalidate(ui::host_invalid::redraw | ui::host_invalid::controller);
	}

	void cancel_region_drag()
	{
		const auto restore = _state.region_drag_restore();
		_state.end_region_drag();
		region(restore);
	}

	photo_control(view_state& state, display_state_ptr display, view_host_ptr host) :
		view_element(view_element_style::can_invoke | flex_item::media), _state(state), _display(std::move(display)),
		_host(std::move(host))
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

	void render_zoom_thumb(render::draw_context& dc, pointi element_offset) const;

	void render(render::draw_context& dc, const pointi element_offset) const override
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
					element->_host->invalidate(ui::host_invalid::redraw);
					return element->_display->zoom_interactive(core::now());
				};
				_host->invalidate(ui::host_invalid::animations);
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
				const core::scope_exit restore_alpha([&dc, original_alpha] { dc.colors.alpha = original_alpha; });
				dc.colors.alpha *= std::min(dc.colors.overlay_alpha, _display->_zoom_overlay_alpha);
				const auto grading_bounds = _zoom_grading_element->bounds.offset(element_offset);
				dc.draw_rect(grading_bounds.inflate(dc.padding1), core::color(0, dc.colors.alpha * 0.5f));
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

	void render_region(render::draw_context& dc, const pointi element_offset) const
	{
		_region_close_bounds.clear();
		_region_zoom_bounds.clear();
		_region_crop_bounds.clear();

		const auto region = region_bounds(element_offset);
		if (region.is_empty()) return;

		const auto border_clr = core::color(ui::style::colors.dialog_selected_background, dc.colors.alpha);
		dc.draw_border(region, region.inflate(core::round(2 * dc.scale_factor)), border_clr, border_clr);

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
		const auto left = std::clamp(inside ? region.right - strip_width - gap : region.left, limit.left,
		                             std::max(limit.left, limit.right - strip_width));
		const auto top = std::clamp(inside ? region.top + gap : region.bottom + gap, limit.top,
		                            std::max(limit.top, limit.bottom - cxy));

		_region_close_bounds = recti(left, top, left + cxy, top + cxy);
		_region_zoom_bounds = _region_close_bounds.offset(cxy + gap, 0);
		_region_crop_bounds = _region_zoom_bounds.offset(cxy + gap, 0);

		const auto strip = recti(_region_close_bounds.left, top, _region_crop_bounds.right, top + cxy);
		dc.draw_rounded_rect(strip.inflate(gap), core::color(0, dc.colors.alpha * 0.66f), dc.padding1);

		const auto clr = core::color(dc.colors.foreground, dc.colors.alpha);
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
			return {core::round(cx), core::round(cy)};
		}

		return {};
	}

	sizei measure(render::measure_context& /*mc*/, const int width_limit) const override
	{
		const auto tex_extent = calc_tex_extent(width_limit, width_limit * 3);
		return {width_limit, tex_extent.cy};
	}

	void layout(render::measure_context& mc)
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
				const recti grading_bounds{grading_limit.right - grading_extent.cx,
				                           grading_limit.bottom - grading_extent.cy, grading_limit.right,
				                           grading_limit.bottom};
				_zoom_grading_element->layout(mc, grading_bounds);
			}
		}
	}

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		bounds = bounds_in;
		layout(mc);
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

// Space plays and pauses it, as a click on it does, so the picture itself is no part of the keyboard's.
class video_control final :
	public view_element,
	public std::enable_shared_from_this<video_control>,
	public no_keyboard_parts
{
public:
	view_state& _state;
	display_state_ptr _display;
	view_host_ptr _host;

	video_control(view_state& s, display_state_ptr display, view_host_ptr host) noexcept :
		view_element(view_element_style::can_invoke | flex_item::media), _state(s), _display(std::move(display)),
		_host(std::move(host))
	{
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
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
				_display->draw_subtitles(dc);
			}
		}
		catch (std::exception& e)
		{
			// A frame that cannot be drawn is dropped rather than propagated out of paint. core::trace
			// is compiled out of a shipping build, so the reason has to reach the log or a blank
			// video is a defect with no evidence; log_once bounds a failure that repeats per frame.
			core::log_once(__FUNCTION__, e.what());
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
			result = sizei(core::round(cx), core::round(cy));
		}

		return result;
	}

	sizei measure(render::measure_context& /*mc*/, const int width_limit) const override
	{
		const auto tex_extent = calc_tex_extent(width_limit, width_limit * 3);
		return {width_limit, tex_extent.cy};
	}

	void layout(render::measure_context& mc, const recti bounds_in) override
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

// An audio file's panel in the media stack: its cover art, when it has one, over the visualizer's band of bars,
// which the display brings up to the playback position for every present. A panel like the others, not a
// picture: fullscreen stacks it with the selection panel rather than giving it the screen. Until the cover's
// picture arrives its place is left empty rather than greyed as a photo's is while it loads. Space plays and pauses
// it, as a click on it does, so the panel itself is no part of the keyboard's.
class audio_control final :
	public view_element,
	public std::enable_shared_from_this<audio_control>,
	public no_keyboard_parts
{
public:
	view_state& _state;
	view_host_ptr _host;
	display_state_ptr _display;
	// Fullscreen has more height to give the band and the art.
	const bool _fullscreen;
	recti _art;
	recti _band;

	audio_control(view_state& s, display_state_ptr display, view_host_ptr host, const bool fullscreen = false) noexcept
		:
		view_element(view_element_style::can_invoke | flex_item::media), _state(s), _host(std::move(host)),
		_display(std::move(display)), _fullscreen(fullscreen)
	{
		// Its panel meets the column's edges, as the selection panel below it does.
		padding(0);
		margin(0);
	}

	// The cover art's shape; empty for a file without any.
	sizei art_dimensions() const
	{
		return _display->_selected_texture1 && _display->player_has_video()
		           ? _display->_selected_texture1->calc_display_dimensions()
		           : sizei{};
	}

	// The band's height in a width: half as tall as it is wide, as the bars are drawn, within limits.
	int band_height(render::measure_context& mc, const int inner_width) const
	{
		const auto most = core::round((_fullscreen ? 420 : 300) * mc.scale_factor);
		return std::clamp(inner_width / 2, core::round(96 * mc.scale_factor), most);
	}

	int art_height(render::measure_context& mc, const int inner_width) const
	{
		const auto art = art_dimensions();
		if (art.cx <= 0 || art.cy <= 0) return 0;
		const auto most = core::round((_fullscreen ? 420 : 320) * mc.scale_factor);
		return std::min({core::mul_div(art.cy, inner_width, art.cx), core::mul_div(inner_width, 3, 5), most});
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
	{
		const auto pad = core::round(ui::panel_element::content_padding * mc.scale_factor);
		const auto inner = std::max(0, width_limit - pad * 2);
		const auto art = art_height(mc, inner);
		return {width_limit, pad * 2 + art + (art > 0 ? pad : 0) + band_height(mc, inner)};
	}

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		bounds = bounds_in;

		// The band keeps at least half the height, which is the panel's to give; the art gives way first.
		const auto pad = core::round(ui::panel_element::content_padding * mc.scale_factor);
		const auto inner = bounds.inflate(-pad);
		const auto band_wanted = band_height(mc, inner.width());
		const auto band = std::min(band_wanted,
		                           std::max(inner.height() / 2, inner.height() - art_height(mc, inner.width()) - pad));
		const auto art_space = std::max(0, inner.height() - band - pad);

		_art = {};
		_band = recti(inner.left, inner.bottom - band, inner.right, inner.bottom);

		if (art_space > 0 && art_height(mc, inner.width()) > 0 && _display->_selected_texture1)
		{
			const recti limit(inner.left, inner.top, inner.right, inner.top + art_space);
			_art = center_rect(core::scale_dimensions(art_dimensions(), limit), limit);
			_display->_selected_texture1->layout(mc, _art, _display->_item1);
		}
		else
		{
			_band = inner;
		}
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		const auto panel = bounds.offset(element_offset);
		draw_stack_panel(dc, panel);

		// The bars' shadows spill past them; the panel holds them.
		const render::scoped_clip clip(dc, panel);

		_display->_audio_panel_bounds = panel;
		_display->_audio_element_offset = element_offset;
		_display->_audio_element_alpha = dc.colors.alpha;
		_display->_audio_element_bounds = _band.inflate(0, -std::max(4, _band.height() / 10));

		if (_display->_selected_texture1)
		{
			const auto st = _display->_selected_texture1;

			_display->media_offset = element_offset;
			_display->update_for_present(dc.time_now);

			if (_display->player_has_video() && !st->_vid_tex)
			{
				st->_vid_tex = dc.create_texture();
			}

			if (_display->player_has_video() && st->_vid_tex && !_art.is_empty() && st->prepare_picture(dc))
			{
				st->draw(dc, element_offset, 0, false);
			}
		}

		// The band's resting line, which the bars rise from and fall to, so a paused file's panel is not empty.
		const auto band = _band.offset(element_offset);
		const auto middle = (band.top + band.bottom) / 2;
		dc.draw_rect(recti(band.left, middle, band.right, middle + std::max(1, core::round(dc.scale_factor))),
		             core::color(ui::style::colors.view_text, dc.colors.alpha * 0.12f));

		if (!_display->_audio_verts)
		{
			_display->_audio_verts = dc.create_vertices();
		}

		if (_display->_audio_verts)
		{
			_display->update_for_present(dc.time_now);
			dc.draw_vertices(_display->_audio_verts);
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

	void render_zoom_thumb(render::draw_context& dc, pointi element_offset) const;

	void render(render::draw_context& dc, const pointi element_offset) const override
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
			const core::scope_exit restore_overlay_alpha([&dc, original_overlay_alpha]
			{ dc.colors.overlay_alpha = original_overlay_alpha; });

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
					const core::scope_exit restore_alpha([&dc, original_alpha] { dc.colors.alpha = original_alpha; });
					dc.colors.alpha *= dc.colors.overlay_alpha;
					const auto grading_bounds = grading->bounds.offset(element_offset);
					dc.draw_rect(grading_bounds.inflate(dc.padding1), core::color(0, dc.colors.alpha * 0.5f));
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
					const auto extent = dc.measure_text(text, render::font_face::title,
					                                    render::text_style::single_line_center, 200);
					const recti letter(client.right - dc.padding2 - std::max(extent.cx, extent.cy),
					                   client.top + dc.padding2, client.right - dc.padding2,
					                   client.top + dc.padding2 + extent.cy);
					_display->_pane_marker_bounds[0] = letter.inflate(dc.padding1);
					dc.draw_rect(_display->_pane_marker_bounds[0], core::color(0, alpha * 0.5f));
					dc.draw_text(text, letter, render::font_face::title, render::text_style::single_line_center,
					             core::color(dc.colors.foreground, alpha), {});
				}
				else if (_pane_marker_height > 0)
				{
					const auto alpha = dc.colors.alpha;
					const auto marker_bottom = client.top + _pane_marker_height;
					const auto underline = std::max(2, core::round(2 * dc.scale_factor));

					for (auto i = 0; i < 2; ++i)
					{
						const std::string_view text = i == 0 ? "A" : "B";
						const auto is_active = can_zoom && (i == 0) == (active == df::zoom_pane::primary);
						const recti label(client.left + core::mul_div(i, client.width(), 2), client.top,
						                  client.left + core::mul_div(i + 1, client.width(), 2), marker_bottom);
						dc.draw_text(text, label, render::font_face::title, render::text_style::single_line_center,
						             core::color(dc.colors.foreground, is_active || !can_zoom ? alpha : alpha * 0.5f),
						             {});

						const auto extent = dc.measure_text(text, render::font_face::title,
						                                    render::text_style::single_line_center, 200);
						const auto width = std::max(extent.cx, underline * 3);
						const auto center_x = label.center().x;
						const recti marker(center_x - width / 2, client.top, center_x - width / 2 + width,
						                   marker_bottom);

						if (can_zoom) _display->_pane_marker_bounds[i] = marker.inflate(dc.padding1);

						if (is_active)
						{
							// The active pane is marked under its own letter, so nothing is drawn over the image.
							dc.draw_rect(recti(marker.left, marker_bottom - underline, marker.right, marker_bottom),
							             core::color(ui::style::colors.view_selected_background, alpha));
						}
					}
				}
			}

			if (_display->_comparing)
			{
				auto split_bounds = bounds.offset(element_offset);
				split_bounds.left = std::max(split_bounds.left, compare_pos - 1);
				split_bounds.right = std::min(split_bounds.right, compare_pos + 1);
				dc.draw_rect(split_bounds, core::color(0, dc.colors.overlay_alpha));

				const auto icon_size = std::max(dc.padding2, dc.scroll_width);
				const auto icon_top = bounds.bottom + element_offset.y - icon_size - dc.padding1;
				xdraw_icon(dc, icon_index::small_left,
				           recti(compare_pos - icon_size, icon_top, compare_pos, icon_top + icon_size),
				           core::color(dc.colors.foreground, dc.colors.overlay_alpha), {});
				xdraw_icon(dc, icon_index::small_right,
				           recti(compare_pos, icon_top, compare_pos + icon_size, icon_top + icon_size),
				           core::color(dc.colors.foreground, dc.colors.overlay_alpha), {});
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
				const auto track_color = core::color(0, dc.colors.alpha / 3.33f);
				const auto handle_color = view_handle_color(false, is_hover, is_tracking, dc.frame_has_focus, true)
				                              .aa(dc.colors.alpha);
				const auto text_color = core::color(dc.colors.foreground, dc.colors.alpha);
				const auto scale1 = core::round(1 * dc.scale_factor);

				for (auto row = 0; row < 2; ++row)
				{
					const auto duration = row == 0 ? duration1 : duration2;
					auto track = scrubber;
					track.top += row * (row_height + row_gap);
					track.bottom = track.top + row_height;
					track.right = track.left + core::mul_div(track.width(), duration, max_duration);

					auto track_background = track;
					if (!is_hover && !is_tracking)
					{
						track_background.top += dc.padding1;
						track_background.bottom -= dc.padding1;
					}
					dc.draw_rounded_rect(track_background, track_color, dc.padding1);

					const auto elapsed = core::mul_div(std::clamp(_display->_compare_video_pos, 0, scrubber.width()),
					                                   max_duration, std::max(1, scrubber.width()));
					const auto playhead = track.left +
					                      core::mul_div(std::min(elapsed, duration), track.width(), duration);
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
					dc.draw_text(core::format_seconds(duration), duration_bounds, render::font_face::dialog,
					             render::text_style::single_line, text_color, {});
				}
			}
			else if (!_display->is_zoom_mode())
			{
				// Only the two-pane arrangement has a divider to grab.
				auto rr = comparison_divider_device_bounds(_display->_compare_bounds, element_offset);
				rr.left = (rr.left + rr.right) / 2;
				rr.right = rr.left + 1;
				dc.draw_rect(rr, core::color(core::average(dc.colors.background, 0), dc.colors.alpha));
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
					result.emplace_back(std::max(1, ww),
					                    std::min(std::max(1, ww), std::max(1, height_limit - reserved_height)));
					continue;
				}
				auto image_scale = calc_thumb_scale(dimensions, sized(ww, std::max(1, height_limit - reserved_height)),
				                                    false);
				if (!setting.scale_up && image_scale > 1.0) image_scale = 1.0;
				const auto cx = std::max(1.0, dimensions.Width * image_scale);
				const auto cy = std::max(1.0, dimensions.Height * image_scale);
				result.emplace_back(core::round(cx), core::round(cy));
			}
		}

		return result;
	}

	// Panes are named in reserved space above the images; a magnified pane is named by an overlay instead.
	int calc_pane_marker_height(render::measure_context& mc) const
	{
		if (!_display->is_comparison() || _display->_comparing || _display->is_zoom_mode()) return 0;
		return mc.measure_text("A", render::font_face::title, render::text_style::single_line_center, 200).cy +
		       mc.padding2;
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
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

			return {width_limit,
			        cy + _pane_marker_height + (_display->_is_compare_video ? compare_timeline_height : 0)};
		}

		return {};
	}

	void layout(render::measure_context& mc) const
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
			core::assert_true(layout_extent.size() == tex_count);

			const std::array<texture_state_ptr, 2> selected_textures{_display->_selected_texture1,
			                                                         _display->_selected_texture2};
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
						tex_bounds.left = bounds.left + core::mul_div(i, bounds.width(), tex_count);
						tex_bounds.right = bounds.left + core::mul_div(i + 1, bounds.width(), tex_count);

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
				const auto duration_text = core::format_seconds(std::max(duration1, duration2));
				_display->_time_width =
					mc.measure_text(duration_text, render::font_face::dialog, render::text_style::single_line, 200).cx;

				_display->_compare_video_control_bounds = bounds;
				_display->_compare_video_control_bounds.top = bounds.bottom - compare_timeline_height;
				_display->_compare_video_scrubber_bounds = _display->_compare_video_control_bounds
				                                               .inflate(-mc.padding2, -mc.padding2);
				_display->_compare_video_scrubber_bounds.top += compare_timeline_top_padding;
				_display->_compare_video_scrubber_bounds.right -= _display->_time_width + mc.padding1;
			}

			auto handle_bounds = bounds;
			handle_bounds.top += _pane_marker_height;
			_display->_compare_bounds = center_rect(sizei{mc.scroll_width, core::mul_div(handle_bounds.height(), 5, 7)},
			                                        handle_bounds);

			if (zoom_mode)
			{
				const auto index = zoomed_pane == df::zoom_pane::primary ? 0u : 1u;
				const auto& grading = _zoom_grading_elements[index];
				if (grading)
				{
					const auto grading_limit = bounds.inflate(-mc.padding2);
					const auto grading_extent = grading->measure(mc, grading_limit.width());
					const recti grading_bounds{grading_limit.right - grading_extent.cx,
					                           grading_limit.bottom - grading_extent.cy, grading_limit.right,
					                           grading_limit.bottom};
					grading->layout(mc, grading_bounds);
				}
			}

			// if (_is_comparing)
			{
				_display->_compare_limits = bounds;

				for (const auto& tex : selected_textures)
				{
					const auto tex_bounds = tex->display_bounds();
					if (_display->_compare_limits.left < tex_bounds.left)
						_display->_compare_limits.left = tex_bounds.left;
					if (_display->_compare_limits.right > tex_bounds.right)
						_display->_compare_limits.right = tex_bounds.right;
				}
			}
		}
	}

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		bounds = bounds_in;
		layout(mc);
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
			return (_display->_compare_limits.left + _display->_compare_limits.right) / 2;
		return std::clamp(_display->_compare_hover_loc, _display->_compare_limits.left + 4,
		                  _display->_compare_limits.right - 4);
	}

	friend class compare_controller;
};

// The selection's thumbnails as a collage, in a panel of the media stack like the others.
class images_control2 final : public view_element, public std::enable_shared_from_this<images_control2>
{
	view_state& _state;
	display_state_ptr _display;
	recti _pin_badge_bounds;

public:
	images_control2(view_state& state, display_state_ptr display) noexcept : _state(state), _display(std::move(display))
	{
		// Its panel meets the column's edges, as the selection panel below it does.
		padding(0);
		margin(0);
	}

	sizei measure(render::measure_context& mc, const int cx) const override
	{
		const auto image_count = _display->_selection_item_count;
		const auto cx_control = std::min(core::round(360 * mc.scale_factor), core::mul_div(cx, 5, 11));

		if (image_count > 0)
		{
			return {cx, std::max(core::round(180 * mc.scale_factor), cx_control)};
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

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		bounds = bounds_in;
		const auto collage_bounds = bounds_in.inflate(
			-core::round(ui::panel_element::content_padding * mc.scale_factor));
		const auto minimum_cell = std::max(1, core::round(96 * mc.scale_factor));
		const auto area_capacity = static_cast<size_t>(
			std::max(1, collage_bounds.area() / (minimum_cell * minimum_cell)));
		const auto cell_capacity = std::clamp(area_capacity, 6_z, display_state_t::max_surfaces);
		const auto visible_without_overflow = std::min(_display->_surfaces.size(), cell_capacity);
		const auto has_overflow = _display->_selection_item_count > visible_without_overflow;
		_display->_collage_image_count = std::min(_display->_surfaces.size(), cell_capacity - (has_overflow ? 1 : 0));
		// Unsigned: a selection smaller than the cells built for it would otherwise read as billions.
		_display->_selection_overflow_count = _display->_selection_item_count > _display->_collage_image_count
		                                          ? _display->_selection_item_count - _display->_collage_image_count
		                                          : 0;
		_display->_surface_bounds = ui::layout_collage(collage_bounds,
		                                               surface_dims(_display->_collage_image_count, has_overflow));

		const auto gutter = std::max(1, core::round(2 * mc.scale_factor));
		for (auto& surface_bounds : _display->_surface_bounds)
		{
			if (surface_bounds.width() > gutter * 2 && surface_bounds.height() > gutter * 2)
			{
				surface_bounds = surface_bounds.inflate(-gutter);
			}
		}

		_pin_badge_bounds = calc_pin_badge_bounds(mc.icon_cxy);
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		draw_stack_panel(dc, bounds.offset(element_offset));

		// Keyed on the surface count rather than on being non-empty, so a set published after the first
		// paint is uploaded rather than half-drawn against a cache built for a shorter one.
		if (_display->_textures.size() != _display->_surfaces.size())
		{
			_display->_textures.clear();
			_display->_textures.reserve(_display->_surfaces.size());

			for (const auto& s : _display->_surfaces)
			{
				auto t = dc.create_texture();

				if (t && t->update(s) == render::texture_update_result::failed)
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
			                                 (texture_dimensions.cy - source_height) / 2, source_width, source_height)
			                               .round();
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

			dc.draw_rect(overflow_bounds, core::color(ui::style::colors.group_background, dc.colors.alpha));
			dc.draw_text(text, overflow_bounds, render::font_face::dialog, render::text_style::single_line_center,
			             core::color(dc.colors.foreground, dc.colors.alpha), {});
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
	                                             const pointi element_offset, hit_test_context& /*ctx*/) override
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
	const core::const_surface_ptr _surface;
	const view_element_ptr _child;
	mutable render::texture_ptr _tex;
	mutable sizei _surface_extent;
	mutable sizei _child_extent;
	recti _surface_bounds;
	recti _child_bounds;

public:
	split_element(core::const_surface_ptr s, view_element_ptr child, const view_element_options& style_in) noexcept :
		view_element(style_in), _surface(std::move(s)), _child(std::move(child))
	{
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		if (is_valid(_surface))
		{
			if (!_tex)
			{
				const auto t = dc.create_texture();

				if (t && t->update(_surface) != render::texture_update_result::failed)
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

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		bounds = bounds_in;

		const auto surface_cy_extra = (bounds_in.height() - _surface_extent.cy) / 2;
		const auto child_cy_extra = (bounds_in.height() - _child_extent.cy) / 2;

		_surface_bounds = recti({bounds_in.left, bounds_in.top + surface_cy_extra}, _surface_extent);
		_child_bounds = recti({bounds_in.left + _surface_extent.cx, bounds_in.top + child_cy_extra}, _child_extent);
		_child->layout(mc, _child_bounds);
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		_child->tooltip(hover, loc, element_offset);
	}

	void hover(interaction_context& ic) override { _child->hover(ic); }

	void dispatch_event(const view_element_event& event) override
	{
		_child->dispatch_event(event);

		if (event.type == view_element_event_type::free_graphics_resources)
		{
			_tex.reset();
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		return _child->controller_from_location(host, loc, element_offset, ctx);
	}

	void visit_children(const std::function<void(const view_element_ptr&)>& visit) const override { visit(_child); }

	sizei measure(render::measure_context& mc, const int width_limit) const override
	{
		_surface_extent = core::scale_dimensions(_surface->dimensions(), width_limit / 2, true);
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
	bullet_element(const icon_index i, view_element_ptr child, const view_element_options& style_in = {}) noexcept :
		view_element(style_in), _icon(i), _child(std::move(child))
	{
	}

	bullet_element(const icon_index i, const std::string_view text) :
		_icon(i), _child(std::make_shared<text_element>(text))
	{
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		auto icon_bounds = bounds.offset(element_offset);
		icon_bounds.right = icon_bounds.left + dc.icon_cxy + _icon_padding;
		icon_bounds.left += 8;

		xdraw_icon(dc, _icon, icon_bounds, core::color(dc.colors.foreground, dc.colors.alpha), {});

		if (_child)
		{
			_child->render(dc, element_offset);
		}
	}

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		bounds = bounds_in;

		const auto child_cy_extra = (bounds_in.height() - _child_extent.cy) / 2;

		_child_bounds = recti({bounds_in.left + mc.icon_cxy + _icon_padding, bounds_in.top + child_cy_extra},
		                      _child_extent);
		_child->layout(mc, _child_bounds);
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		_child->tooltip(hover, loc, element_offset);
	}

	void hover(interaction_context& ic) override { _child->hover(ic); }

	void dispatch_event(const view_element_event& event) override { _child->dispatch_event(event); }

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             const pointi element_offset, hit_test_context& ctx) override
	{
		return _child->controller_from_location(host, loc, element_offset, ctx);
	}

	void visit_children(const std::function<void(const view_element_ptr&)>& visit) const override
	{
		if (_child) visit(_child);
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
	{
		const auto w = mc.icon_cxy + _icon_padding;
		_child_extent = _child->measure(mc, width_limit - w);
		return {_child_extent.cx + w, std::max(mc.icon_cxy, _child_extent.cy)};
	}
};
