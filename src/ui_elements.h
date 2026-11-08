// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The application's own elements on libdesktop UI's element tree: the explainer that
// opens a view's controls panel, centred action text, and a surface drawn in the orientation the
// user's settings ask for. The tree itself, and the basic elements, are libdesktop's.

#pragma once

#include "libdesktop/ui.h"
#include "ui_view.h"

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

class action_element final : public std::enable_shared_from_this<action_element>, public text_element_base
{
public:
	explicit action_element(const std::string_view t) : text_element_base(
		t, flex_item::center | flex_item::new_line)
	{
		_font = render::font_face::dialog;
		_text_style = render::text_style::multiline;
		_foreground_clr = ui::style::colors.dialog_text;
	}

	explicit action_element(std::string&& t) : text_element_base(
		t, flex_item::center | flex_item::new_line)
	{
		_font = render::font_face::dialog;
		_text_style = render::text_style::multiline;
		_foreground_clr = ui::style::colors.dialog_text;
	}
};

class surface_element final : public view_element
{
	const core::const_surface_ptr _surface;
	mutable render::texture_ptr _tex;
	int _max_size = 0;
	core::orientation _orientation = core::orientation::top_left;

public:
	surface_element(core::const_surface_ptr s, const int max_size, const view_element_options& options,
	                const core::orientation orientation = core::orientation::top_left) noexcept :
		view_element(options), _surface(std::move(s)), _max_size(max_size), _orientation(orientation)
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
				const auto destination = bounds.offset(element_offset);
				if (setting.show_rotated && _orientation != core::orientation::top_left)
				{
					dc.draw_texture(_tex, quadd(destination).transform(to_simple_transform(_orientation)),
					                recti(_tex->dimensions()), dc.colors.alpha, render::texture_sampler::point);
				}
				else
				{
					dc.draw_texture(_tex, destination);
				}
			}
		}
	}

	sizei measure(render::measure_context& mc, const int width_limit) const override
	{
		if (!is_valid(_surface)) return {};
		auto dimensions = _surface->dimensions();
		if (setting.show_rotated && flips_xy(_orientation)) std::swap(dimensions.cx, dimensions.cy);
		return core::scale_dimensions(dimensions,
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


