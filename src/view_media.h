// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Media viewing and playback. Displays photos and videos with
// playback controls, comparison view, and metadata display.

#pragma once

inline view_element_ptr make_icon_link_element2(const icon_index i, commands cmd, const view_element_options& style_in)
{
	auto element = std::make_shared<link_element>(icon_to_utf8(i), cmd, ui::style::font_face::icons,
	                                              ui::style::text_style::single_line_center, style_in, true);
	return element;
}

// design.md: the media overlay identifies rather than summarises, and this is the invariant that
// stops it returning to a summary. Never more than a quarter of the media's height, truncated rather
// than grown. The floor keeps a small window showing the identification rather than nothing at all.
// Where the media is not a picture - audio, an archive, a file the viewer cannot decode - the panel
// is the presentation rather than a caption on one, so it may take half.
constexpr int calc_media_overlay_height(const int measured, const int media_height, const int min_height,
                                        const bool panel_is_the_presentation = false)
{
	const auto limit = panel_is_the_presentation ? media_height / 2 : media_height / 4;
	return std::min(measured, std::max(min_height, limit));
}

class media_view final : public view_base
{
	df::zoom_view_state _touch_pan_start_zoom;
	using this_type = media_view;

	view_state& _state;
	view_host_ptr _host;

	view_element_ptr _left_arrow_element;
	view_element_ptr _right_arrow_element;
	view_element_ptr _media_element;
	view_elements_ptr _controls_element;
	view_element_ptr _description_element;
	display_state_ptr _display;

	sizei _client_extent;

public:
	float overlay_alpha = 1.0f;
	float overlay_alpha_target = 1.0f;

	media_view(view_state& state, view_host_ptr host) :
		_state(state),
		_host(std::move(host)),
		_left_arrow_element(make_icon_link_element2(icon_index::left, commands::browse_previous_item,
		                                            view_element_style::none)),
		_right_arrow_element(make_icon_link_element2(icon_index::right, commands::browse_next_item,
		                                             view_element_style::none))
	{
	}

	~media_view() override
	{
		df::log(__FUNCTION__, "destruct");
	}

	recti calc_media_bounds() const
	{
		return {0, 0, _client_extent.cx, _client_extent.cy};
	}

	void activate(const sizei extent) override
	{
		_client_extent = extent;
		update_media_elements();
	}

	void deactivate() override
	{
	}

	void refresh() override
	{
		_state.open(_host, _state.search(), {});
	}

	menu_type context_menu(const pointi loc) override
	{
		return menu_type::media;
	}

	// zoom.md 11: fullscreen has one surface, so the modifier alone chooses which of its axes moves.
	bool mouse_wheel(const pointi loc, const ui::wheel_notch notch) override
	{
		if (!_display) return false;

		const auto magnified = _display->is_temporary_zoom() || _display->is_zoom_mode();

		if (magnified)
		{
			// Horizontal is the secondary axis in every magnified state, inspect zoom included: a tilt is
			// not a magnification gesture, and its steps come from the other accumulator anyway.
			if (notch.is_horizontal())
			{
				_display->active_zoom_pane_at(pointd(loc));
				_display->pan_zoom_by({-notch.delta / 2.0, 0.0});
				return true;
			}

			// Inspect zoom is a gesture the user is already holding, so its wheel adjusts the temporary
			// magnification whatever the modifier says; in durable zoom mode Ctrl is the ladder.
			if (_display->is_temporary_zoom() || notch.keys.control)
			{
				return step_zoom(loc, notch.steps);
			}
		}
		else if (notch.keys.control)
		{
			// Not an entry into zoom mode. Consumed so the cheapest possible input cannot reach
			// something else's scale instead.
			return true;
		}

		if (!notch.is_vertical()) return false;
		if (notch.delta == 0) return true;
		_state.select_next(_host, notch.delta <= 0, false, notch.keys.shift);
		return true;
	}

	bool pinch(const pointi loc, const int steps) override
	{
		return _display && _display->can_zoom() && step_zoom(loc, steps);
	}

	bool step_zoom(const pointi loc, const int steps)
	{
		if (steps == 0) return true;
		_display->active_zoom_pane_at(pointd(loc));
		const auto anchor = _display->zoom_anchor_at(pointd(loc));
		for (auto step = 0; step < std::abs(steps); ++step)
			_display->adjust_zoom_scale(steps > 0 ? 1 : -1, anchor);
		return true;
	}

	void pan_start(const pointi start_loc) override
	{
		if (_display) _touch_pan_start_zoom = _display->zoom_state();
	}

	void pan(const pointi start_loc, const pointi current_loc) override
	{
		if (_display && _display->zoom())
			_display->pan_zoom(pointd(current_loc - start_loc), _touch_pan_start_zoom);
	}

	void pan_end(const pointi start_loc, const pointi final_loc) override
	{
		pan(start_loc, final_loc);
	}

	bool touch_double_tap(const pointi location) override
	{
		if (!_display || !_display->can_zoom()) return false;
		_display->active_zoom_pane_at(pointd(location));
		if (_display->zoom()) _display->zoom(false);
		else _display->zoom_100(_display->zoom_anchor_at(pointd(location)));
		return true;
	}

	void render(ui::draw_context& dc, view_controller_ptr controller) override
	{
		const auto original_colors = dc.colors;
		dc.colors.overlay_alpha = overlay_alpha;
		dc.colors.bg_alpha = overlay_alpha * 0.55f;

		if (_media_element)
		{
			_media_element->render(dc, {0, 0});
		}

		if (_display && !_display->is_zoom_mode() && !_display->comparing())
		{
			dc.colors.alpha = dc.colors.alpha * overlay_alpha;

			if (_controls_element && !_controls_element->bounds.is_empty())
			{
				// The overlay is laid out into at most a quarter of the media height, which is less
				// than it measured, so it truncates rather than growing. Without the clip the excess
				// paints over the picture the view exists to show. The clip admits the rounded
				// background the element paints outside its bounds, as the description's does.
				const auto pad = _controls_element->padding * dc.scale_factor;
				const ui::scoped_clip clip(dc, _controls_element->bounds.inflate(pad.cx, pad.cy));
				_controls_element->render(dc, {0, 0});
			}
			if (_description_element && !_description_element->bounds.is_empty())
			{
				// The clip caps overlong prose, so it must admit the rounded background the element
				// paints outside its bounds or the panel loses its corners.
				const auto pad = _description_element->padding * dc.scale_factor;
				const ui::scoped_clip clip(dc, _description_element->bounds.inflate(pad.cx, pad.cy));
				_description_element->render(dc, {0, 0});
			}

			_left_arrow_element->render(dc, {0, 0});
			_right_arrow_element->render(dc, {0, 0});

			dc.colors.alpha = original_colors.alpha;

			const auto is_slide_show = _display->is_playing_slideshow();

			if (is_slide_show)
			{
				const auto alpha = dc.colors.alpha * (1.0f - dc.colors.overlay_alpha);
				const auto scale_factor = dc.scale_factor;

				auto progress_bounds = calc_media_bounds().inflate(df::round(-20 * scale_factor));
				progress_bounds.top = progress_bounds.bottom - df::round(16 * scale_factor);
				progress_bounds.right = progress_bounds.left + df::round(64 * scale_factor);

				dc.draw_rect(progress_bounds, ui::color(ui::style::color::group_background, alpha * 0.7f));

				auto r = progress_bounds.inflate(df::round(-2 * scale_factor));
				r.right = r.left + df::mul_div(std::min(_display->slideshow_pos(), 1000), r.width(), 1000);
				dc.draw_rect(r, ui::color(ui::style::color::view_selected_background, alpha * 0.7f));
			}
		}

		dc.colors = original_colors;
	}

	void layout(ui::measure_context& mc, const sizei extent) override
	{
		_client_extent = extent;

		ui::control_layouts positions;
		auto avail_bounds = calc_media_bounds();
		const auto scale_factor = mc.scale_factor;

		if (!_state.is_full_screen || !_display || !_display->is_one())
		{
			avail_bounds = avail_bounds.inflate(df::round(-4 * scale_factor));
		}

		const int minimum_media_control_width = df::round(32 * 7 * scale_factor);
		// Zoom and compare give the whole client to the media element, as items_view does, so its
		// navigator and rating overlays stay in the client corners after a resize.
		const auto media_owns_client = _display && (_display->is_zoom_mode() || _display->comparing());
		const auto overlay_media_control = media_owns_client || (_display && _display->is_one() &&
			_display->display_item_has_trait(file_traits::hide_overlays));
		auto media_bounds_avail = avail_bounds;
		if (_controls_element) _controls_element->bounds.clear();
		if (_description_element) _description_element->bounds.clear();

		if (avail_bounds.width() > minimum_media_control_width && _controls_element)
		{
			const auto control_limit = avail_bounds.inflate(-mc.padding2);
			const auto panel_gap = mc.padding2;
			const auto controls_width = std::min(control_limit.width(),
			                                     std::max(df::round(360 * scale_factor),
			                                              df::mul_div(control_limit.width(), 5, 11)));
			const auto controls_extent = _controls_element->measure(mc, controls_width);
			// design.md: the overlay identifies rather than summarises, and this is the bound that
			// stops it growing back into a summary as a file turns out to carry more metadata. An item
			// with no picture of its own reserves space for the panel instead of floating over the media,
			// and the panel is then what the view is showing, so it is allowed twice as much.
			const auto panel_is_the_presentation = !overlay_media_control && _display && _display->is_one();
			const auto controls_height = calc_media_overlay_height(controls_extent.cy, avail_bounds.height(),
			                                                       mc.icon_cxy * 2, panel_is_the_presentation);
			const auto controls_left = (control_limit.left + control_limit.right - controls_width) / 2;
			const recti control_bounds{
				controls_left, control_limit.bottom - controls_height,
				controls_left + controls_width, control_limit.bottom
			};

			_controls_element->layout(mc, control_bounds, positions);

			// The description takes only the free space between the controls and the right edge, so
			// showing one never narrows the controls or moves them.
			auto panel_height = controls_height;
			const auto description_avail = control_limit.right - control_bounds.right - panel_gap;
			const auto min_description_width = df::round(220 * scale_factor);

			if (_description_element && description_avail >= min_description_width)
			{
				const auto description_width = std::min(description_avail, df::round(420 * scale_factor));
				const auto description_extent = _description_element->measure(mc, description_width);
				const auto max_description_height = std::max(df::round(112 * scale_factor), avail_bounds.height() / 3);
				const auto description_height = std::min(description_extent.cy, max_description_height);
				const recti description_bounds{
					control_limit.right - description_width, control_limit.bottom - description_height,
					control_limit.right, control_limit.bottom
				};
				_description_element->layout(mc, description_bounds, positions);
				panel_height = std::max(panel_height, description_height);
			}

			if (!overlay_media_control)
			{
				media_bounds_avail.bottom = control_limit.bottom - panel_height - mc.padding2;
			}
		}

		const auto arrow_cx = avail_bounds.width() / 17;
		const auto arrow_cy = avail_bounds.height() / 7;
		const auto left_top = (avail_bounds.top + avail_bounds.bottom - arrow_cy) / 2;
		const auto right_top = (avail_bounds.top + avail_bounds.bottom - arrow_cy) / 2;

		_left_arrow_element->layout(
			mc, recti(avail_bounds.left, left_top, avail_bounds.left + arrow_cx, left_top + arrow_cy), positions);
		_right_arrow_element->layout(mc, recti(avail_bounds.right - arrow_cx, right_top, avail_bounds.right,
		                                       right_top + arrow_cy), positions);

		if (!overlay_media_control)
		{
			media_bounds_avail.left += arrow_cx + mc.padding2;
			media_bounds_avail.right -= arrow_cx + mc.padding2;
		}

		if (_media_element)
		{
			auto media_bounds = media_bounds_avail;

			if (!media_owns_client)
			{
				const auto media_extent = _media_element->measure(mc, media_bounds_avail.width());
				media_bounds = recti(media_bounds_avail.top_left(), media_extent);

				if (media_bounds.height() < media_bounds_avail.height() || media_bounds.width() < media_bounds_avail.
					width())
				{
					media_bounds = center_rect(media_bounds, media_bounds_avail);
				}
				else if (_media_element->flex.shrink > 0.0f && (media_bounds.height() >
					media_bounds_avail.height() || media_bounds.width() > media_bounds_avail.width()))
				{
					media_bounds = media_bounds_avail;
				}
			}

			_media_element->layout(mc, media_bounds, positions);
		}

		_host->frame()->invalidate();
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, const pointi loc,
	                                             hit_test_context& ctx) override
	{
		constexpr pointi media_offset{};

		if (_display && (_display->is_zoom_mode() || _display->comparing()))
		{
			return _media_element ? _media_element->controller_from_location(host, loc, media_offset, ctx) : nullptr;
		}

		// The arrows float on the picture, so they are opaque only where they actually answer; asking
		// first and recording the miss is what keeps the picture usable when they are not offered.
		for (const auto& arrow : std::array<view_element_ptr, 2>{_left_arrow_element, _right_arrow_element})
		{
			if (!arrow) continue;
			if (auto controller = arrow->controller_from_location(host, loc, media_offset, ctx)) return controller;
			ctx.passed_over(arrow->bounds);
		}

		// The panels are drawn backgrounds, so they are opaque throughout: a pointer on one must not
		// reach the picture underneath even where the panel itself answers nothing.
		for (const auto& panel : std::array<view_element_ptr, 2>{_controls_element, _description_element})
		{
			if (panel && ctx.occluded(panel->bounds))
			{
				return panel->controller_from_location(host, loc, media_offset, ctx);
			}
		}

		return _media_element ? _media_element->controller_from_location(host, loc, media_offset, ctx) : nullptr;
	}

	// design.md L5: Escape peels one layer. A region can be drawn here too, so clearing it comes
	// before the unwind ladder that would otherwise leave fullscreen with the rectangle still on the
	// picture.
	bool escape() override
	{
		if (const auto photo = std::dynamic_pointer_cast<photo_control>(_media_element); photo && photo->has_region())
		{
			photo->clear_region();
			return true;
		}

		return false;
	}

	void display_changed() override
	{
		_state.invalidate_view(view_invalid::view_layout);
	}

	void update_media_elements() override;

	void broadcast_event(const view_element_event& event) const override
	{
		if (_left_arrow_element) _left_arrow_element->dispatch_event(event);
		if (_right_arrow_element) _right_arrow_element->dispatch_event(event);
		if (_controls_element) _controls_element->dispatch_event(event);
		if (_description_element) _description_element->dispatch_event(event);
		if (_media_element) _media_element->dispatch_event(event);
	}
};
