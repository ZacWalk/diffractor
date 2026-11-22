// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The application's own elements on libdesktop UI's element tree: the explainer that
// opens a view's controls panel, centred action text, a surface drawn in the orientation the
// user's settings ask for (or upright with a face marked), and the common panel's background.
// The tree itself, and the basic elements, are libdesktop's.

#pragma once

#include "libdesktop/ui.h"
#include "ui_view.h"

// The common panel's background, for an element that draws its panels itself: the shade and corners
// libdesktop's panel_element draws, so they read as panels of the same stack.
inline void draw_stack_panel(render::draw_context& dc, const recti r)
{
	static const auto shade = std::make_shared<ui::panel_element>()->_bg_color;
	auto c = shade;
	c.a *= dc.colors.alpha;
	dc.draw_rounded_rect(r, c, dc.padding1);
}

// The task or selection, then its effect. Both lines are ordinary elements so they wrap, respond
// to DPI changes and are readable by the same tools as the controls that follow.
class view_info_element final : public view_elements
{
	icon_index _icon;
	std::shared_ptr<text_element> _heading;
	std::shared_ptr<text_element> _detail;
	recti _badge;

	int arrange(render::measure_context& mc, int width, pointi origin, bool place) const;

public:
	view_info_element(icon_index icon, std::string_view heading, std::string_view detail);
	bool heading(std::string_view value);
	bool text(std::string_view value);
	sizei measure(render::measure_context& mc, int width) const override;
	void layout(render::measure_context& mc, recti bounds_in) override;
	void render(render::draw_context& dc, pointi offset) const override;
};

class action_element final : public std::enable_shared_from_this<action_element>, public text_element_base
{
public:
	explicit action_element(const std::string_view t) : text_element_base(t, flex_item::line_break | flex_item::new_line)
	{
		_font = render::font_face::dialog;
		_text_style = render::text_style::multiline;
		_foreground_clr = ui::style::tone_text(ui::style::tone::accent);
	}

};

void add_shortcut_hint(view_hover_element& hover, std::string_view keys);
void add_shortcut_hint(view_hover_element& hover, const ui::command_ptr& command);

// A picture, turned as the surface's own orientation says - what its pixels were stored with, which for a thumbnail
// need not be what the picture it stands for records - with its longer side at most max_size, or with none set at most
// the width offered.
class surface_element final : public view_element
{
	const core::const_surface_ptr _surface;
	mutable render::texture_ptr _tex;
	int _max_size = 0;
	sizei _photo_limit;
	std::optional<rectd> _upright_face;

	sizei displayed_dimensions() const
	{
		auto size = _surface->dimensions();
		if ((_upright_face || setting.show_rotated) && flips_xy(_surface->orientation())) std::swap(size.cx, size.cy);
		return size;
	}

public:
	surface_element(core::const_surface_ptr s, const int max_size, const view_element_options& options) noexcept :
		view_element(options), _surface(std::move(s)), _max_size(max_size)
	{
	}

	surface_element(core::const_surface_ptr s, const sizei photo_limit, const rectd upright_face) noexcept :
		view_element(flex_item::center | flex_item::shrink), _surface(std::move(s)), _photo_limit(photo_limit),
		_upright_face(upright_face)
	{
	}

	void render(render::draw_context& dc, const pointi element_offset) const override
	{
		if (is_valid(_surface) && !bounds.is_empty())
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
				const auto orientation = _surface->orientation();
				if ((_upright_face || setting.show_rotated) && orientation != core::orientation::top_left)
				{
					dc.draw_texture(_tex, quadd(destination).transform(to_simple_transform(orientation)),
					                recti(_tex->dimensions()), dc.colors.alpha,
					                _upright_face ? render::texture_sampler::bilinear : render::texture_sampler::point);
				}
				else
				{
					dc.draw_texture(_tex, destination);
				}
				if (_upright_face && _upright_face->Width > 0 && _upright_face->Height > 0)
				{
					const auto& f = *_upright_face;
					const recti box(core::round(destination.left + f.X * destination.width()),
					                core::round(destination.top + f.Y * destination.height()),
					                core::round(destination.left + f.right() * destination.width()),
					                core::round(destination.top + f.bottom() * destination.height()));
					const auto outer = box.intersection(destination);
					if (!outer.is_empty())
					{
						const auto blue = core::color(core::rgba(55, 157, 255), dc.colors.alpha);
						const auto stroke = std::max(1, core::round(2 * dc.scale_factor));
						if (std::min(outer.width(), outer.height()) <= 2 * stroke) dc.draw_rect(outer, blue);
						else dc.draw_border(outer.inflate(-stroke), outer, blue, blue);
					}
				}
			}
		}
	}

	sizei measure(render::measure_context& /*mc*/, const int width_limit) const override
	{
		if (!is_valid(_surface)) return {};
		auto dimensions = displayed_dimensions();
		if (_upright_face)
			return core::scale_dimensions(dimensions, sizei{std::max(1, std::min(width_limit, _photo_limit.cx)),
			                                               _photo_limit.cy});
		dimensions = core::scale_dimensions(dimensions, _max_size > 0 ? _max_size : width_limit, true);

		// Within max_size the width offered limits the width alone. A flex line measures again at the width it has just
		// given, and holding the longer side to that width shrank a portrait each time and drew it squashed into a
		// square - so a picture laid out in one has a max_size.
		if (dimensions.cx > width_limit)
			dimensions = core::scale_dimensions(dimensions, sizei{std::max(1, width_limit), dimensions.cy}, true);
		return dimensions;
	}

	void layout(render::measure_context& mc, const recti bounds_in) override
	{
		// On a short screen the bubble's column gives the photo what remains after its captions.
		view_element::layout(mc, _upright_face && is_valid(_surface) && !bounds_in.is_empty()
		                             ? core::scale_dimensions(displayed_dimensions(), bounds_in)
		                             : bounds_in);
	}

	void dispatch_event(const view_element_event& event) override
	{
		if (event.type == view_element_event_type::free_graphics_resources)
		{
			_tex.reset();
		}
	}
};
