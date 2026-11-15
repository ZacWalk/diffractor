// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The stack's panels as monitors draw them, in the shade and corners of libdesktop's common panel,
// and the ordinary transport used by every media presentation.

#pragma once

#include "ui_elements.h"
#include "model_display.h"

// Diffractor's own palette as the monitors draw with it: translucent shades that sink a part into its panel or
// raise a control off it, the view's text, its selection blue for what is lit, and its success, important and
// warning colours for gates and meters, solo and mute.
namespace monitor_palette
{
	struct shade
	{
		core::color32 rgb;
		float alpha;
	};

	constexpr shade sunken{0x000000, 0.3f};
	constexpr shade raised{0xFFFFFF, 0.12f};
	constexpr shade line{0xFFFFFF, 0.1f};

	// clang-format off
	inline core::color32 text() { return ui::style::colors.view_text; }
	inline core::color32 text_muted() { return core::lerp(ui::style::colors.view_text, ui::style::colors.view_background, 0.35f); }
	inline core::color32 text_faint() { return core::lerp(ui::style::colors.view_text, ui::style::colors.view_background, 0.6f); }
	inline core::color32 accent() { return ui::style::colors.view_selected_background; }
	inline core::color32 accent_light() { return core::lerp(ui::style::colors.view_selected_background, ui::style::colors.view_text, 0.4f); }
	inline core::color32 information() { return ui::style::colors.info_background; }
	inline core::color32 green() { return ui::style::colors.success_background; }
	inline core::color32 orange() { return ui::style::colors.important_background; }
	inline core::color32 red() { return ui::style::colors.warning_background; }
	// clang-format on
}

// One of a monitor's panels, as a child of its flex tree: its extent and, with background, the common
// panel's shade behind it.
class music_panel_element final : public view_element
{
	sizei _extent;
	bool _background;

public:
	music_panel_element(const sizei extent, const bool background, const view_element_options& options = {}) :
		view_element(options), _extent(extent), _background(background)
	{
		// Panels meet the column's edges and each other's gap exactly, as the stack's other panels do.
		padding(0);
		margin(0);
	}

	sizei measure(render::measure_context&, const int width_limit) const override
	{
		return {std::min(_extent.cx, width_limit), _extent.cy};
	}

	void render(render::draw_context& dc, const pointi offset) const override
	{
		if (_background) draw_stack_panel(dc, bounds.offset(offset));
	}
};

// Geometry may compact while the measured font stays at the user's chosen size.
class music_measure_context final : public render::measure_context
{
	render::measure_context& _source;

public:
	music_measure_context(render::measure_context& source, const double scale) : _source(source)
	{
		scale_factor = scale;
		padding1 = source.padding1;
		padding2 = source.padding2;
		scroll_width = source.scroll_width;
		icon_cxy = source.icon_cxy;
	}

	sizei measure_text(const std::string_view text, const render::font_face font, const render::text_style style,
	                   const int width, const int height = 0) override
	{
		return _source.measure_text(text, font, style, width, height);
	}
	int text_line_height(const render::font_face font) override { return _source.text_line_height(font); }
	render::text_layout_ptr create_text_layout(const render::font_face font) override
	{
		return _source.create_text_layout(font);
	}
};

view_elements_ptr make_media_transport(view_state& state, const display_state_ptr& display);
