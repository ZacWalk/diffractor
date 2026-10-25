// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Base view elements and building blocks. Defines text, icons, links, dividers,
// thumbnails, tables, and other fundamental UI components.

#pragma once

#include "libdesktop/ui.h"
#include "ui_view.h"

enum class view_element_style : uint32_t
{
	none = 0,
	checked = 1 << 16,

	visible = 1 << 17,
	can_invoke = 1 << 18,
	has_tooltip = 1 << 19,

	hover = 1 << 20,
	tracking = 1 << 21,
	selected = 1 << 22,
	error = 1 << 23,
	highlight = 1 << 24,

	important = 1 << 25,
	info = 1 << 26,
	background = 1 << 27,
	dark_background = 1 << 28,
	shaded_background = 1 << 29,
};

constexpr view_element_style operator|(const view_element_style a, const view_element_style b)
{
	return static_cast<view_element_style>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

constexpr view_element_style operator&(const view_element_style a, const view_element_style b)
{
	return static_cast<view_element_style>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

constexpr view_element_style operator^(const view_element_style a, const view_element_style b)
{
	return static_cast<view_element_style>(static_cast<uint32_t>(a) ^ static_cast<uint32_t>(b));
}

constexpr view_element_style operator~(const view_element_style a)
{
	return static_cast<view_element_style>(~static_cast<uint32_t>(a));
}

constexpr bool operator&&(const view_element_style a, const view_element_style b)
{
	return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

constexpr view_element_style& operator|=(view_element_style& a, const view_element_style b)
{
	a = a | b;
	return a;
}

constexpr view_element_style& operator&=(view_element_style& a, const view_element_style b)
{
	a = a & b;
	return a;
}

constexpr view_element_style& operator^=(view_element_style& a, const view_element_style b)
{
	a = a ^ b;
	return a;
}

struct view_element_padding
{
	int16_t cx = 0;
	int16_t cy = 0;

	void operator()(const int16_t xy)
	{
		cx = cy = xy;
	}

	void operator()(const int16_t x, const int16_t y)
	{
		cx = x;
		cy = y;
	}

	operator sizei() const
	{
		return {cx, cy};
	}

	sizei operator *(const double d) const noexcept
	{
		return {df::round(cx * d), df::round(cy * d)};
	}
};

enum class flex_direction
{
	row,
	column
};

enum class flex_wrap
{
	no_wrap,
	wrap
};

enum class flex_justify
{
	start,
	center,
	end,
	space_between
};

enum class flex_align
{
	automatic,
	start,
	center,
	end,
	stretch
};

struct flex_item_layout
{
	// All lengths are logical units; calc_flex_layout applies the measure-context scale once.
	float grow = 0.0f;
	float shrink = 0.0f;
	int basis = -1;
	sizei min_size{};
	sizei max_size{INT_MAX, INT_MAX};
	flex_align align_self = flex_align::automatic;
	bool main_start_auto = false;
	bool break_before = false;
	bool break_after = false;
	bool keep_with_next = false;
	bool center_line = false;
};

struct flex_container_layout
{
	// Gap and padding use the same logical-unit contract as item basis/min/max sizes.
	flex_direction direction = flex_direction::row;
	flex_wrap wrap = flex_wrap::wrap;
	flex_justify justify = flex_justify::start;
	flex_align align_items = flex_align::center;
	sizei gap{};
	sizei padding{};
};

constexpr flex_item_layout operator|(flex_item_layout left, const flex_item_layout& right)
{
	left.grow = std::max(left.grow, right.grow);
	left.shrink = std::max(left.shrink, right.shrink);
	if (right.basis >= 0) left.basis = right.basis;
	left.min_size.cx = std::max(left.min_size.cx, right.min_size.cx);
	left.min_size.cy = std::max(left.min_size.cy, right.min_size.cy);
	left.max_size.cx = std::min(left.max_size.cx, right.max_size.cx);
	left.max_size.cy = std::min(left.max_size.cy, right.max_size.cy);
	if (right.align_self != flex_align::automatic) left.align_self = right.align_self;
	left.main_start_auto |= right.main_start_auto;
	left.break_before |= right.break_before;
	left.break_after |= right.break_after;
	left.keep_with_next |= right.keep_with_next;
	left.center_line |= right.center_line;
	return left;
}

namespace flex_item
{
	inline constexpr auto grow = []
	{
		flex_item_layout result;
		result.grow = 1.0f;
		return result;
	}();
	inline constexpr auto shrink = []
	{
		flex_item_layout result;
		result.shrink = 1.0f;
		return result;
	}();
	// A media pane yields space to the controls beside it, but never below a height at which the
	// image or video stops being recognisable.
	inline constexpr auto media = []
	{
		flex_item_layout result;
		result.shrink = 1.0f;
		result.min_size.cy = 64;
		return result;
	}();
	inline constexpr auto stretch = []
	{
		flex_item_layout result;
		result.align_self = flex_align::stretch;
		return result;
	}();
	inline constexpr auto center = []
	{
		flex_item_layout result;
		result.align_self = flex_align::center;
		result.center_line = true;
		return result;
	}();
	inline constexpr auto right_justified = []
	{
		flex_item_layout result;
		result.main_start_auto = true;
		return result;
	}();
	inline constexpr auto line_break = []
	{
		flex_item_layout result;
		result.break_after = true;
		return result;
	}();
	inline constexpr auto no_break = []
	{
		flex_item_layout result;
		result.keep_with_next = true;
		return result;
	}();
	inline constexpr auto new_line = []
	{
		flex_item_layout result;
		result.break_before = true;
		return result;
	}();
}

struct view_element_options
{
	view_element_style style = view_element_style::none;
	flex_item_layout flex;

	constexpr view_element_options() = default;

	constexpr view_element_options(const view_element_style style_in) : style(style_in)
	{
	}

	constexpr view_element_options(const flex_item_layout& flex_in) : flex(flex_in)
	{
	}
};

constexpr view_element_options operator|(view_element_options left, const view_element_options& right)
{
	left.style |= right.style;
	left.flex = left.flex | right.flex;
	return left;
}

constexpr view_element_options operator|(const view_element_style left, const flex_item_layout& right)
{
	return view_element_options(left) | view_element_options(right);
}

constexpr view_element_options operator|(const flex_item_layout& left, const view_element_style right)
{
	return view_element_options(left) | view_element_options(right);
}

constexpr view_element_options operator|(const view_element_options& left, const view_element_style right)
{
	return left | view_element_options(right);
}

constexpr view_element_options operator|(const view_element_style left, const view_element_options& right)
{
	return view_element_options(left) | right;
}

constexpr view_element_options operator|(const view_element_options& left, const flex_item_layout& right)
{
	return left | view_element_options(right);
}

constexpr view_element_options operator|(const flex_item_layout& left, const view_element_options& right)
{
	return view_element_options(left) | right;
}


class view_element
{
public:
	// invalidate_slack value meaning "not known yet, or wider than a byte can describe", which makes
	// invalidate_bounds ask for the whole frame.
	static constexpr uint8_t unknown_invalidate_slack = 255;

	recti bounds;
	view_element_padding padding{3, 3};
	view_element_padding margin{0, 0};
	flex_item_layout flex;
	view_element_style style = view_element_style::visible;

	// How far outside bounds this element paints, in device pixels. Maintained at draw time because
	// padding is logical and only becomes device pixels once the scale factor is known.
	mutable uint8_t invalidate_slack = unknown_invalidate_slack;

	ui::color _bg_color;
	ui::color _bg_target;

	view_element() noexcept = default;

	constexpr explicit view_element(const view_element_style style_in) noexcept : style(
		style_in | view_element_style::visible)
	{
	}

	constexpr explicit view_element(const view_element_options& options) noexcept :
		flex(options.flex), style(options.style | view_element_style::visible)
	{
	}

	view_element(const view_element&) noexcept = delete;
	view_element& operator=(const view_element&) noexcept = delete;
	view_element(view_element&&) noexcept = delete;
	view_element& operator=(view_element&&) noexcept = delete;
	virtual ~view_element() noexcept = default;

	ui::color calc_background_color(const ui::draw_context& dc) const
	{
		auto c = _bg_color;
		c.a = _bg_color.a * dc.colors.alpha;
		return c;
	}

	// The rectangle a partial repaint must cover to redraw this element completely. Empty means the
	// whole frame, which is what an unknown slack falls back to.
	recti invalidate_bounds(const pointi element_offset = {}) const
	{
		if (invalidate_slack == unknown_invalidate_slack) return {};
		return bounds.offset(element_offset).inflate(invalidate_slack);
	}

	void render_background(ui::draw_context& dc, const pointi element_offset) const
	{
		const auto pad = padding * dc.scale_factor;

		// draw_rounded_rect grows its fill by round(0.8333 * (radius + 2) - radius), which is at most
		// 2 for any radius. Recorded even when nothing is drawn, so the first hover already knows it.
		const auto slack = std::max(pad.cx, pad.cy) + 2;
		invalidate_slack = slack < unknown_invalidate_slack
			                   ? static_cast<uint8_t>(slack)
			                   : unknown_invalidate_slack;

		const auto bg = calc_background_color(dc);

		if (bg.a > 0.0f)
		{
			dc.draw_rounded_rect(bounds.offset(element_offset).inflate(pad.cx, pad.cy), bg, dc.padding1);
		}
	}

	sizei porch() const
	{
		return {padding.cx + margin.cx, padding.cy + margin.cy};
	}

	virtual bool is_control_area(const pointi loc, const pointi element_offset) const
	{
		return is_visible() && bounds.contains(loc - element_offset) && (can_invoke() || has_tooltip());
	}

	virtual void render(ui::draw_context& dc, const pointi element_offset) const
	{
	}

	virtual sizei measure(ui::measure_context& mc, const int width_limit) const
	{
		return {};
	}

	virtual void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions)
	{
		bounds = bounds_in;
	}

	virtual void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const
	{
	}

	virtual void hover(interaction_context& ic)
	{
	}

	virtual void dispatch_event(const view_element_event& event)
	{
	}

	virtual void visit_controls(const std::function<void(const ui::control_base_ptr&)>& handler)
	{
	}

	virtual view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                                     hit_test_context& ctx);

	bool is_style_bit_set(const view_element_style mask) const
	{
		return style && mask;
	}

	void set_style_bit(view_element_style mask, bool state, const view_host_base_ptr& view, const view_element_ptr& e);
	void update_background_color();
	void set_style_bit(view_element_style mask, bool state);

	bool can_invoke() const
	{
		return is_style_bit_set(view_element_style::can_invoke);
	}

	bool has_tooltip() const
	{
		return is_style_bit_set(view_element_style::has_tooltip);
	}

	bool is_visible() const
	{
		return is_style_bit_set(view_element_style::visible);
	}

	void is_visible(const bool visible)
	{
		set_style_bit(view_element_style::visible, visible);
	}
};

struct flex_layout_result
{
	std::vector<recti> layout_bounds;
	sizei extent;
};

flex_layout_result calc_flex_layout(const std::vector<view_element_ptr>& elements, ui::measure_context& mc,
                                    sizei available, const flex_container_layout& container);

sizei layout_flex_elements(const std::vector<view_element_ptr>& elements, ui::measure_context& mc,
                           ui::control_layouts& positions, recti bounds, const flex_container_layout& container);

class view_elements : public view_element, public ui::bubble_content, public std::enable_shared_from_this<view_elements>
{
protected:
	std::vector<view_element_ptr> _children;

public:
	flex_container_layout flex_container;

	view_elements() noexcept = default;

	explicit view_elements(const view_element_options& options) noexcept : view_element(options)
	{
	}

	explicit view_elements(std::vector<view_element_ptr> children) noexcept : _children(std::move(children))
	{
	}

	bool is_empty() const
	{
		return _children.empty();
	}

	void clear()
	{
		_children.clear();
	}

	size_t size() const
	{
		return _children.size();
	}

	bool is_control_area(const pointi loc, const pointi element_offset) const override
	{
		if (!is_visible()) return false;

		for (const auto& c : _children)
		{
			if (c->is_control_area(loc, element_offset))
			{
				return true;
			}
		}

		return false;
	}

	void render(ui::draw_context& dc, const pointi offset) const override
	{
		if (std::ranges::any_of(_children, [](const view_element_ptr& e) { return e->is_visible(); }))
		{
			render_background(dc, offset);

			for (const auto& e : _children)
			{
				if (e->is_visible())
				{
					e->render(dc, offset);
				}
			}
		}
	}

	void add(const view_element_ptr& v)
	{
		_children.emplace_back(v);
	}

	void add(const std::vector<view_element_ptr>& vals)
	{
		if (!vals.empty())
		{
			_children.insert(_children.end(), vals.begin(), vals.end());
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return calc_flex_layout(_children, mc, {width_limit, -1}, container_layout(mc)).extent;
	}

	void layout(ui::measure_context& mc, const recti bounds_in, ui::control_layouts& positions) override
	{
		update_style_from_children();
		bounds = bounds_in;
		layout_flex_elements(_children, mc, positions, bounds, container_layout(mc));
	}

	void update_style_from_children()
	{
		auto has_tooltip = false;
		auto can_invoke = false;

		for (const auto& c : _children)
		{
			has_tooltip |= c->has_tooltip();
			can_invoke |= c->can_invoke();
		}

		set_style_bit(view_element_style::has_tooltip, has_tooltip);
		set_style_bit(view_element_style::can_invoke, can_invoke);
	}

	void tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const override
	{
		for (const auto& c : _children)
		{
			if (c->is_visible() && c->bounds.offset(element_offset).contains(loc))
			{
				c->tooltip(hover, loc, element_offset);
			}
		}
	}

	void hover(interaction_context& ic) override
	{
		for (const auto& c : _children)
		{
			c->hover(ic);
		}
	}


	void dispatch_event(const view_element_event& event) override
	{
		for (const auto& c : _children)
		{
			c->dispatch_event(event);
		}
	}

	void visit_controls(const std::function<void(const ui::control_base_ptr&)>& handler) override
	{
		for (const auto& c : _children)
		{
			c->visit_controls(handler);
		}
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;

protected:
	// Metrics such as a gap in device pixels are only known once there is a measure context, so a
	// derived container folds them in here rather than caching a scale-dependent value.
	virtual flex_container_layout container_layout(const ui::measure_context& mc) const
	{
		return flex_container;
	}
};

struct view_hover_element
{
	static constexpr int default_preferred_size = 300;

	view_elements_ptr elements;
	recti window_bounds;
	recti active_bounds;
	uint32_t active_delay = 0;
	int x_focus = -1;
	int preferred_size = default_preferred_size;
	bool horizontal = false;
	commands id = {};

	view_hover_element() = default;

	bool is_empty() const
	{
		return elements == nullptr || elements->is_empty();
	}

	void clear()
	{
		elements = std::make_shared<view_elements>();
		window_bounds.clear();
		active_bounds.clear();
		x_focus = -1;
		preferred_size = default_preferred_size;
		horizontal = false;
		id = {};
	}

	friend class ui_popup_controler;
};


class text_element_base : public view_element
{
protected:
	std::string _text;
	ui::style::font_face _font = ui::style::font_face::dialog;
	ui::style::text_style _text_style = ui::style::text_style::multiline;
	ui::color32 _foreground_clr = 0;

	mutable ui::text_layout_ptr _tl;

public:
	text_element_base(const std::string_view text, const view_element_options& options = {}) noexcept :
		view_element(options), _text(text)
	{
	}

	void foreground_color(const ui::color32 clr)
	{
		_foreground_clr = clr;
	}

	void text(const std::string_view t)
	{
		_text = t;

		if (_tl)
		{
			_tl->update(_text, _text_style);
		}
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		render_background(dc, element_offset);
		draw_text_content(dc, bounds.offset(element_offset));
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		const auto& tl = text_layout(mc);
		return tl ? tl->measure_text(width_limit) : sizei{};
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::dpi_changed)
		{
			_tl.reset();
		}
	}

protected:
	// Text layout creation can fail when the graphics device is lost, so every caller checks the result.
	const ui::text_layout_ptr& text_layout(ui::measure_context& mc) const
	{
		if (!_tl)
		{
			_tl = mc.create_text_layout(_font);
			if (_tl) _tl->update(_text, _text_style);
		}

		return _tl;
	}

	void draw_text_content(ui::draw_context& dc, const recti logical_bounds) const
	{
		const auto& tl = text_layout(dc);

		if (tl)
		{
			const auto clr = _foreground_clr
				                 ? ui::color(_foreground_clr, dc.colors.alpha)
				                 : ui::color(dc.colors.foreground, dc.colors.alpha);

			dc.draw_text(tl, logical_bounds, clr, {});
		}
	}
};

class text_element final : public std::enable_shared_from_this<text_element>, public text_element_base
{
public:
	text_element(const std::string_view text, const ui::style::font_face font, const ui::style::text_style text_style,
	             const view_element_options& options) noexcept : text_element_base(text, options)
	{
		_font = font;
		_text_style = text_style;

		update_background_color();
	}

	text_element(const std::string_view text, const ui::style::text_style text_style) noexcept : text_element_base(
		text)
	{
		_text_style = text_style;
	}

	text_element(const std::string_view text, const ui::style::font_face font) noexcept : text_element_base(text)
	{
		_font = font;
	}

	text_element(const std::string_view text, const view_element_options& options) noexcept : text_element_base(
		text, options)
	{
	}

	text_element(const std::string_view text) noexcept : text_element_base(text)
	{
	}
};

// The explainer that opens a view's controls panel. The shaded band and its own padding set it
// apart from the controls below, so it never needs a divider under it.
inline std::shared_ptr<text_element> create_view_info_element(const std::string_view text)
{
	auto result = std::make_shared<text_element>(text, view_element_style::shaded_background | flex_item::stretch);
	result->padding = {12, 12};
	result->margin = {0, 8};
	result->update_background_color();
	return result;
}

inline ui::color32 link_foreground_color()
{
	return ui::average(ui::style::color::dialog_text, ui::style::color::dialog_selected_background);
}


class action_element final : public std::enable_shared_from_this<action_element>, public text_element_base
{
public:
	explicit action_element(const std::string_view t) : text_element_base(
		t, flex_item::center | flex_item::new_line)
	{
		_font = ui::style::font_face::dialog;
		_text_style = ui::style::text_style::multiline;
		_foreground_clr = ui::style::color::dialog_text;
	}

	explicit action_element(std::string&& t) : text_element_base(
		t, flex_item::center | flex_item::new_line)
	{
		_font = ui::style::font_face::dialog;
		_text_style = ui::style::text_style::multiline;
		_foreground_clr = ui::style::color::dialog_text;
	}
};

class link_element final : public std::enable_shared_from_this<link_element>, public text_element_base
{
	commands _cmd = {};
	std::function<void()> _invoke;
	std::function<void(view_hover_element&)> _tooltip;
	bool _full_background = false;

public:
	link_element(const std::string_view text, const commands cmd, const ui::style::font_face font,
	             const ui::style::text_style text_style,
	             const view_element_options& options = {},
	             const bool full_background = false) noexcept : text_element_base(
		                                                            text, options), _cmd(cmd),
	                                                            _full_background(full_background)
	{
		_font = font;
		_text_style = text_style;
		update_style();
	}

	link_element(const std::string_view text, std::function<void()> func, const ui::style::font_face font,
	             const ui::style::text_style text_style,
	             const view_element_options& options = {},
	             const bool full_background = false) noexcept :
		text_element_base(text, options), _invoke(std::move(func)), _full_background(full_background)
	{
		_font = font;
		_text_style = text_style;
		update_style();
	}

	link_element(const std::string_view text, std::function<void()> func,
	             std::function<void(view_hover_element&)> tooltip,
	             const ui::style::font_face font, const ui::style::text_style text_style,
	             const view_element_options& options = {}) noexcept :
		text_element_base(text, options), _invoke(std::move(func)), _tooltip(std::move(tooltip))
	{
		_font = font;
		_text_style = text_style;
		update_style();
	}

	link_element(const std::string_view text, const commands cmd) noexcept : text_element_base(text), _cmd(cmd)
	{
		update_style();
	}

	link_element(const std::string_view text, std::function<void()> func) noexcept : text_element_base(text),
		_invoke(std::move(func))
	{
		update_style();
	}

	link_element(const std::string_view text) noexcept : text_element_base(text)
	{
		update_style();
	}

	void update_style()
	{
		set_style_bit(view_element_style::has_tooltip, _cmd != commands{} || _tooltip);
		set_style_bit(view_element_style::can_invoke, _cmd != commands{} || _invoke);

		_foreground_clr = link_foreground_color();
	}

	// locations.md 6.3: a timeline node is a box, not a run of underlined words, so a link may be
	// asked to fill its own background while still carrying a hover bubble.
	void full_background(const bool v)
	{
		_full_background = v;
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		if (_full_background)
		{
			const auto logical_bounds = bounds.offset(element_offset);
			dc.draw_rounded_rect(logical_bounds, calc_background_color(dc), dc.padding1);
			draw_text_content(dc, logical_bounds);
		}
		else
		{
			text_element_base::render(dc, element_offset);
		}
	}

	void update_check(const view_element_event& event);
	void dispatch_event(const view_element_event& event) override;
	void tooltip(view_hover_element& hover, pointi loc, pointi element_offset) const override;

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc, pointi element_offset,
	                                             hit_test_context& ctx) override;
};

inline std::string icon_to_utf8(const icon_index i)
{
	std::string result;
	str::char32_to_utf8(std::back_inserter(result), static_cast<uint32_t>(i));
	return result;
}

inline std::string icon_to_utf8(const icon_index i, const size_t repeat)
{
	const auto single = icon_to_utf8(i);
	std::string text;
	text.reserve(single.size() * repeat);
	for (size_t n = 0; n < repeat; ++n) text += single;
	return text;
}

inline view_element_ptr make_icon_element(const icon_index i, const view_element_options& options)
{
	return std::make_shared<text_element>(icon_to_utf8(i), ui::style::font_face::icons,
	                                      ui::style::text_style::single_line_center, options);
}

inline view_element_ptr make_icon_element(const icon_index i, const size_t repeat, const view_element_options& options)
{
	return std::make_shared<text_element>(icon_to_utf8(i, repeat), ui::style::font_face::icons,
	                                      ui::style::text_style::single_line_center, options);
}

inline view_element_ptr make_icon_link_element(const icon_index i, commands cmd, const view_element_options& options)
{
	return std::make_shared<link_element>(icon_to_utf8(i), cmd, ui::style::font_face::icons,
	                                      ui::style::text_style::single_line_center, options);
}

inline view_element_ptr make_icon_link_element(const icon_index i, const size_t repeat,
                                               const std::function<void()>& func,
                                               const std::function<void(view_hover_element&)>& tooltip)
{
	return std::make_shared<link_element>(icon_to_utf8(i, repeat), func, tooltip, ui::style::font_face::icons,
	                                      ui::style::text_style::single_line_center, view_element_style::none);
}

inline void xdraw_icon(ui::draw_context& dc, const icon_index i, const recti bounds, const ui::color c,
                       const ui::color bg, const ui::style::font_face font = ui::style::font_face::icons)
{
	dc.draw_text(icon_to_utf8(i), bounds, font, ui::style::text_style::single_line_center, c, bg);
}

class surface_element final : public view_element
{
	const ui::const_surface_ptr _surface;
	mutable ui::texture_ptr _tex;
	int _max_size = 0;
	ui::orientation _orientation = ui::orientation::top_left;

public:
	surface_element(ui::const_surface_ptr s, const int max_size, const view_element_options& options,
	                const ui::orientation orientation = ui::orientation::top_left) noexcept :
		view_element(options), _surface(std::move(s)), _max_size(max_size), _orientation(orientation)
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
				const auto destination = bounds.offset(element_offset);
				if (setting.show_rotated && _orientation != ui::orientation::top_left)
				{
					dc.draw_texture(_tex, quadd(destination).transform(to_simple_transform(_orientation)),
					                recti(_tex->dimensions()), dc.colors.alpha, ui::texture_sampler::point);
				}
				else
				{
					dc.draw_texture(_tex, destination);
				}
			}
		}
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		if (!is_valid(_surface)) return {};
		auto dimensions = _surface->dimensions();
		if (setting.show_rotated && flips_xy(_orientation)) std::swap(dimensions.cx, dimensions.cy);
		return ui::scale_dimensions(dimensions,
		                            _max_size == 0 ? width_limit : std::min(_max_size, width_limit), true);
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::free_graphics_resources)
		{
			_tex.reset();
		}
	}
};


class divider_element final : public std::enable_shared_from_this<divider_element>, public view_element
{
public:
	divider_element() noexcept : view_element(flex_item::stretch)
	{
	}

	void render(ui::draw_context& dc, const pointi element_offset) const override
	{
		const auto line_height = df::round(1.1 * dc.scale_factor);
		auto logical_bounds = bounds.offset(element_offset);
		logical_bounds.top = (logical_bounds.top + logical_bounds.bottom - line_height) / 2;
		logical_bounds.bottom = logical_bounds.top + line_height;

		const auto clr = ui::color(0x000000, dc.colors.alpha / 4.44f);
		dc.draw_rect(logical_bounds, clr);
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {width_limit, mc.padding2};
	}
};

class padding_element final : public std::enable_shared_from_this<padding_element>, public view_element
{
public:
	padding_element(const int height = 0) noexcept : view_element(flex_item::grow | flex_item::stretch), _height(height)
	{
	}

	sizei measure(ui::measure_context& mc, const int width_limit) const override
	{
		return {width_limit, _height ? _height : mc.padding2};
	}

	int _height;
};
