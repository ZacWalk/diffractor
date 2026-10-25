// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Out-of-line implementations of the view elements and the media controls: the collage
// layout, the compare, preview and zoom controllers and the zoom overlay, links, ratings, previews and
// the scroller.

#include "pch.h"
#include "libdesktop/util_geometry.h"
#include "model.h"
#include "model_index.h"
#include "ui_elements.h"
#include "ui_dialog.h"
#include "ui_controls.h"

std::vector<recti> ui::layout_collage(const recti draw_bounds, const std::vector<sizei>& dimensions)
{
	constexpr size_t max_cells = 24;
	constexpr double golden_ratio = 1.6180339887498948482;
	const auto cell_count = std::min(dimensions.size(), max_cells);
	std::vector<recti> results(cell_count);

	if (cell_count == 0 || draw_bounds.is_empty()) return {};
	if (cell_count == 1)
	{
		results.front() = draw_bounds;
		return results;
	}

	std::vector<double> aspect_ratios;
	aspect_ratios.reserve(cell_count);
	for (auto index = 0u; index < cell_count; ++index)
	{
		const auto dimensions_safe = dimensions[index];
		aspect_ratios.emplace_back(dimensions_safe.cx > 0 && dimensions_safe.cy > 0
			                           ? static_cast<double>(dimensions_safe.cx) / dimensions_safe.cy
			                           : 1.0);
	}

	const auto canvas_aspect = static_cast<double>(draw_bounds.width()) / draw_bounds.height();
	const auto feature = static_cast<size_t>(std::min_element(aspect_ratios.begin(), aspect_ratios.end(),
	                                                          [canvas_aspect](const double left, const double right)
	                                                          {
		                                                          return std::abs(std::log(left / canvas_aspect)) <
			                                                          std::abs(std::log(right / canvas_aspect));
	                                                          }) - aspect_ratios.begin());

	std::vector<double> weights(cell_count, 1.0);
	weights[feature] = golden_ratio * golden_ratio;
	if (cell_count >= 6)
	{
		weights[(feature + cell_count / 2) % cell_count] = golden_ratio;
	}

	const auto weight_sum = [&weights](const size_t start, const size_t end)
	{
		return std::accumulate(weights.begin() + start, weights.begin() + end, 0.0);
	};

	const auto layout = [&](const auto& self, const size_t start, const size_t end, const recti bounds) -> void
	{
		if (end - start == 1)
		{
			results[start] = bounds;
			return;
		}

		const auto total_weight = weight_sum(start, end);
		auto split = start + 1;
		auto best_balance = std::numeric_limits<double>::max();
		for (auto candidate = start + 1; candidate < end; ++candidate)
		{
			const auto balance = std::abs(weight_sum(start, candidate) / total_weight - 0.5);
			if (balance < best_balance)
			{
				best_balance = balance;
				split = candidate;
			}
		}

		const auto first_fraction = weight_sum(start, split) / total_weight;
		if (bounds.width() > 1 && (bounds.width() >= bounds.height() || bounds.height() <= 1))
		{
			const auto split_x = std::clamp(bounds.left + df::round(bounds.width() * first_fraction),
			                                bounds.left + 1, bounds.right - 1);
			self(self, start, split, {bounds.left, bounds.top, split_x, bounds.bottom});
			self(self, split, end, {split_x, bounds.top, bounds.right, bounds.bottom});
		}
		else if (bounds.height() > 1)
		{
			const auto split_y = std::clamp(bounds.top + df::round(bounds.height() * first_fraction),
			                                bounds.top + 1, bounds.bottom - 1);
			self(self, start, split, {bounds.left, bounds.top, bounds.right, split_y});
			self(self, split, end, {bounds.left, split_y, bounds.right, bounds.bottom});
		}
		else
		{
			for (auto i = start; i < end; ++i) results[i] = bounds;
		}
	};

	layout(layout, 0, cell_count, draw_bounds);

	return results;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


class compare_controller final : public view_controller
{
public:
	std::shared_ptr<side_by_side_control> _parent;
	bool _tracking = false;

	compare_controller(const view_host_ptr& host, std::shared_ptr<side_by_side_control> mc,
	                   const recti bounds) : view_controller(host, bounds), _parent(std::move(mc))
	{
	}

	~compare_controller() override
	{
		_tracking = false;
		_parent->compare(0, _tracking);
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::left_right;
	}

	void draw(ui::draw_context& dc) override
	{
		const auto handle_color = view_handle_color(false, true, _tracking, dc.frame_has_focus, true);
		const auto hover_alpha = _alpha * 0.77f;
		const auto hover_color = handle_color.aa(hover_alpha);

		if (!_tracking)
		{
			dc.draw_rounded_rect(_bounds, hover_color, dc.padding1);
		}
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_tracking = true;
		_parent->compare(loc.x, _tracking);
		_parent->_display->_async.invalidate_view(view_invalid::tooltip);
	}

	void on_mouse_move(const pointi loc) override
	{
		_parent->compare(loc.x, _tracking);
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		_tracking = false;
		_parent->compare(loc.x, _tracking);
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		_parent->compare(0, _tracking);
		return true;
	}

	void popup_from_location(view_hover_element& hover) override
	{
		if (!_tracking)
		{
			hover.elements->add(make_icon_element(icon_index::compare, flex_item::no_break));
			hover.elements->add(std::make_shared<text_element>(tt.compare));
			hover.elements->add(std::make_shared<text_element>(tt.compare_tooltip));
			hover.preferred_size = df::mul_div(hover.preferred_size, 5, 8);
			hover.window_bounds = hover.active_bounds = _bounds;
		}
	}
};

class preview_controller final : public view_controller
{
	std::shared_ptr<side_by_side_control> _parent;
	recti _scrubber_bounds;
	bool _tracking = false;
	bool _hover = false;

public:
	preview_controller(const view_host_ptr& host, std::shared_ptr<side_by_side_control> mc,
	                   const recti control_bounds, const recti scrubber_bounds) :
		view_controller(host, control_bounds), _parent(std::move(mc)), _scrubber_bounds(scrubber_bounds)
	{
	}

	~preview_controller() override
	{
		_tracking = false;
		_hover = false;
		_parent->set_style_bit(view_element_style::tracking, false);
		_parent->set_style_bit(view_element_style::hover, false);

		_parent->_display->_async.invalidate_view(view_invalid::view_redraw);
	}

	void reset(const recti bounds)
	{
		_bounds = bounds;
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::link;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_hover = _bounds.contains(loc);
		_tracking = true;
		_parent->set_style_bit(view_element_style::tracking, true);
		_parent->set_style_bit(view_element_style::hover, _hover);
		on_mouse_move(loc);
	}

	void on_mouse_move(const pointi loc) override
	{
		_hover = _bounds.contains(loc);
		_parent->set_style_bit(view_element_style::hover, _hover);

		if (_tracking)
		{
			const auto pos = std::clamp(loc.x - _scrubber_bounds.left, 0, _scrubber_bounds.width());
			_parent->_display->_compare_video_pos = pos;
			_parent->_display->load_compare_preview(pos, _scrubber_bounds.width());
			_parent->_display->_async.invalidate_view(view_invalid::view_redraw);
		}
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		_hover = _bounds.contains(loc);
		_tracking = false;
		_parent->set_style_bit(view_element_style::tracking, false);
		_parent->set_style_bit(view_element_style::hover, _hover);
	}
};

class zoom_controller final : public view_controller
{
public:
	std::shared_ptr<photo_control> _parent_element;

	view_state& _state;
	recti _view_bounds;
	pointi _element_offset;
	bool _tracking = false;
	bool _drawing_region = false;
	bool _inspect_active = false;
	df::zoom_view_state _start_zoom_state;
	bool _committed = false;

	zoom_controller(const view_host_ptr& host, std::shared_ptr<photo_control> media_parent, view_state& state,
	                const recti interaction_bounds, const recti view_bounds, const pointi element_offset) :
		view_controller(host, interaction_bounds),
		_parent_element(std::move(media_parent)),
		_state(state),
		_view_bounds(view_bounds),
		_element_offset(element_offset),
		_start_zoom_state(_parent_element->_display->zoom_state())
	{
	}

	~zoom_controller() override
	{
		// An interrupted drag would otherwise leave _region_dragging set, and the region's buttons are
		// hidden while it is - permanently, since nothing else clears it.
		if (_drawing_region) _parent_element->cancel_region_drag();
		else if (_tracking && !_committed) _parent_element->_display->restore_zoom_state(_start_zoom_state);
		_tracking = false;
	}

	ui::style::cursor cursor() const override
	{
		if (_drawing_region) return ui::style::cursor::select;
		if (_inspect_active) return ui::style::cursor::size_all;
		// zoom.md: the cursor follows state, not the next pointer move. Ctrl is what arms drawing a
		// region, so it has to be answered before the press rather than after it.
		if (!_tracking && ui::current_key_state().control) return ui::style::cursor::select;
		return ui::style::cursor::zoom;
	}

	void draw(ui::draw_context& dc) override
	{
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_tracking = true;
		_last_loc = _start_loc = loc;
		_inspect_active = false;
		_committed = false;

		// design.md: Ctrl and drag draws a region on the picture. It is read here rather than when the
		// controller was chosen, because the modifier is only known at the moment of the press.
		_drawing_region = keys.control;
		if (_drawing_region)
		{
			_parent_element->begin_region_drag();
			return;
		}

		const auto local = loc - _view_bounds.top_left();
		_parent_element->_display->inspect_at_100(pointd(local));
		_inspect_active = true;
	}

	void on_mouse_move(const pointi loc) override
	{
		if (!_tracking) return;
		_last_loc = loc;

		if (_drawing_region)
		{
			_parent_element->drag_region(_start_loc, loc, _element_offset);
			return;
		}

		const auto local = loc - _view_bounds.top_left();
		const auto extent = _view_bounds.extent();

		// zoom.md: the same positional traversal, applied to whatever is being shown. Over a
		// projection the viewport maps the sphere rather than the rectangle.
		if (_parent_element->_display->is_panorama_projected())
		{
			_parent_element->_display->panorama_look_at(pointd(local));
			return;
		}

		_parent_element->_display->zoom_center({
			std::clamp(local.x / static_cast<double>(std::max(1, extent.cx)), 0.0, 1.0),
			std::clamp(local.y / static_cast<double>(std::max(1, extent.cy)), 0.0, 1.0)
		});
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		// Escape may already have ended the gesture and restored the view. The release still arrives --
		// the button was down when it was cancelled -- so completing here would restore a second time,
		// and for a cancelled region draw would restore a zoom state captured when this controller was
		// built rather than when the drag began.
		const auto was_tracking = _tracking;
		_tracking = false;

		if (!was_tracking)
		{
			_drawing_region = false;
			_inspect_active = false;
			_committed = false;
			return;
		}

		if (_drawing_region)
		{
			_drawing_region = false;
			_parent_element->end_region_drag(_start_loc, loc, _element_offset);
			return;
		}

		if (_committed)
		{
			_committed = false;
		}
		else
		{
			_parent_element->_display->restore_zoom_state(_start_zoom_state);
		}
		_inspect_active = false;
	}

	bool key_down(const char32_t key, const ui::key_state keys) override
	{
		if (_tracking && _inspect_active && key == keys::SPACE)
		{
			_parent_element->_display->commit_inspect();
			_committed = true;
			return true;
		}
		return false;
	}

	bool escape() override
	{
		if (_drawing_region)
		{
			_drawing_region = false;
			_tracking = false;
			_parent_element->cancel_region_drag();
			return true;
		}
		if (!_tracking && !_inspect_active) return false;
		_parent_element->_display->restore_zoom_state(_start_zoom_state);
		_tracking = false;
		_inspect_active = false;
		return true;
	}
};

static void render_zoom_overlay(ui::draw_context& dc, const display_state_ptr& display,
                                const texture_state_ptr& texture_state, const recti bounds, const pointi element_offset,
                                recti& navigator_bounds, recti& fit_bounds, recti& out_bounds, recti& actual_bounds,
                                recti& in_bounds, recti& options_bounds, recti& projection_bounds)
{
	navigator_bounds.clear();
	fit_bounds.clear();
	out_bounds.clear();
	actual_bounds.clear();
	in_bounds.clear();
	options_bounds.clear();
	projection_bounds.clear();
	try
	{
		if (setting.zoom_navigator == zoom_navigator_mode::off) return;
		if (!texture_state) return;

		const auto client_bounds = bounds.offset(element_offset);
		const auto inspect = display->is_temporary_zoom();
		const auto projected = display->is_panorama_projected();
		// zoom.md: there is no one-to-one inside a sphere, so the ladder is a field of view and the
		// readout says so rather than quoting a percentage that would mean nothing.
		const auto scale_text = projected
			                        ? std::format("{:.0f}°", display->panorama().view.fov_degrees())
			                        : std::format("{}%", display->zoom_scale_percent());
		const auto text = !inspect && texture_state->is_provisional()
			                  ? scale_text + " - Preview"
			                  : scale_text;
		const auto text_extent = dc.measure_text(text, ui::style::font_face::dialog,
		                                         ui::style::text_style::single_line, 100);
		const auto label_padding = dc.padding1;
		const auto label_height = text_extent.cy + label_padding * 2;
		const auto available_height = std::max(1, client_bounds.height() - label_height);
		const auto navigator_limit = df::round(df::zoom_view_state::navigator_dip * dc.scale_factor);
		const auto zoom_dim = std::max(32, std::min({navigator_limit, client_bounds.width(), available_height}));
		const auto source_orientation = texture_state->display_orientation();
		const auto source_dims = texture_state->display_dimensions();
		const auto zoom_dims = ui::scale_dimensions(flips_xy(source_orientation) ? source_dims.flip() : source_dims,
		                                            zoom_dim);
		const auto panel_width = inspect ? zoom_dims.cx : zoom_dim;
		const auto panel_bounds = recti(client_bounds.left, client_bounds.top + label_height,
		                                client_bounds.left + panel_width,
		                                client_bounds.top + label_height + zoom_dims.cy)
			.crop(client_bounds);
		const auto zoom_left = panel_bounds.left + (panel_bounds.width() - zoom_dims.cx) / 2;
		const auto zoom_bounds = recti({zoom_left, panel_bounds.top}, zoom_dims).crop(panel_bounds);
		navigator_bounds = zoom_bounds;
		const auto media_bounds = texture_state->display_bounds().offset(element_offset);
		if (media_bounds.is_empty()) return;

		// zoom.md L10: where the visible region sits within the whole. Projected, that region is the
		// patch of the file the camera covers rather than a window onto a scaled rectangle, so it is
		// asked of the camera; the map underneath is the same flat strip either way.
		const auto shown_bounds = projected
			                          ? [&]
			                          {
				                          const auto covered = display->panorama().view.covered_source_fraction(
					                          sized(media_bounds.extent()), display->panorama().geometry);
				                          return recti(
					                          zoom_bounds.left + df::round(covered.X * zoom_bounds.width()),
					                          zoom_bounds.top + df::round(covered.Y * zoom_bounds.height()),
					                          zoom_bounds.left + df::round(covered.right() * zoom_bounds.width()),
					                          zoom_bounds.top + df::round(covered.bottom() * zoom_bounds.height())
				                          ).crop(zoom_bounds);
			                          }()
			                          : [&]
			                          {
				                          const auto l = df::mul_div(client_bounds.left - media_bounds.left,
				                                                     zoom_bounds.width(), media_bounds.width()) +
					                          zoom_bounds.left;
				                          const auto t = df::mul_div(client_bounds.top - media_bounds.top,
				                                                     zoom_bounds.height(), media_bounds.height()) +
					                          zoom_bounds.top;
				                          const auto r = df::mul_div(client_bounds.right - media_bounds.left,
				                                                     zoom_bounds.width(), media_bounds.width()) +
					                          zoom_bounds.left;
				                          const auto b = df::mul_div(client_bounds.bottom - media_bounds.top,
				                                                     zoom_bounds.height(), media_bounds.height()) +
					                          zoom_bounds.top;
				                          return recti(l, t, r, b).crop(zoom_bounds);
			                          }();
		const auto zoom_texture = texture_state->zoom_texture(dc, df::zoom_view_state::navigator_surface_extent);
		if (!zoom_texture) return;

		const auto alpha = std::min(dc.colors.overlay_alpha, display->_zoom_overlay_alpha);
		const auto thumb_alpha = alpha;
		const auto control_bounds = panel_bounds;
		const auto label_width = inspect
			                         ? std::min(panel_bounds.width(), text_extent.cx + label_padding * 2)
			                         : panel_bounds.width();
		const auto label_bounds = recti(panel_bounds.left, client_bounds.top, panel_bounds.left + label_width,
		                                client_bounds.top + label_height).crop(client_bounds);
		const auto has_projection_toggle = !inspect && display->declares_equirectangular();
		const auto button_count = has_projection_toggle ? 6 : 5;
		const auto button_width = std::min(label_height, label_bounds.width() / button_count);
		if (!inspect)
		{
			fit_bounds = recti(label_bounds.left, label_bounds.top,
			                   std::min(label_bounds.right, label_bounds.left + button_width), label_bounds.bottom);
			out_bounds = recti(fit_bounds.right, label_bounds.top,
			                   std::min(label_bounds.right, fit_bounds.right + button_width), label_bounds.bottom);
			options_bounds = recti(std::max(label_bounds.left, label_bounds.right - button_width), label_bounds.top,
			                       label_bounds.right, label_bounds.bottom);

			if (has_projection_toggle)
			{
				projection_bounds = recti(std::max(label_bounds.left, options_bounds.left - button_width),
				                          label_bounds.top, options_bounds.left, label_bounds.bottom);
			}

			const auto right_of_in = has_projection_toggle ? projection_bounds.left : options_bounds.left;
			in_bounds = recti(std::max(label_bounds.left, right_of_in - button_width), label_bounds.top,
			                  right_of_in, label_bounds.bottom);
			actual_bounds = recti(out_bounds.right, label_bounds.top, in_bounds.left, label_bounds.bottom);
		}
		const auto excluded = ui::color(0, thumb_alpha / 2.0f);
		const auto sampler = calc_sampler(zoom_bounds.extent(), zoom_texture->dimensions(),
		                                  zoom_texture->_orientation);
		const auto dst_quad = setting.show_rotated
			                      ? quadd(zoom_bounds).transform(to_simple_transform(zoom_texture->_orientation))
			                      : quadd(zoom_bounds);

		dc.draw_rect(label_bounds, ui::color(0, alpha * 0.5f));
		const auto foreground = ui::color(dc.colors.foreground, alpha);
		if (inspect)
		{
			dc.draw_text(text, label_bounds, ui::style::font_face::dialog,
			             ui::style::text_style::single_line_center, foreground, {});
		}
		else
		{
			dc.draw_text(icon_to_utf8(icon_index::fit), fit_bounds, ui::style::font_face::icons,
			             ui::style::text_style::single_line_center, foreground, {});
			dc.draw_text(icon_to_utf8(icon_index::zoom_out), out_bounds, ui::style::font_face::icons,
			             ui::style::text_style::single_line_center, foreground, {});
			dc.draw_text(text, actual_bounds, ui::style::font_face::dialog,
			             ui::style::text_style::single_line_center, foreground, {});
			dc.draw_text(icon_to_utf8(icon_index::zoom_in), in_bounds, ui::style::font_face::icons,
			             ui::style::text_style::single_line_center, foreground, {});

			// zoom.md L6: which of the two ways of looking at this file is on, shown rather than
			// remembered. Dimmed while the flat pixels are being shown, lit while projected.
			if (has_projection_toggle)
			{
				dc.draw_text(icon_to_utf8(icon_index::world), projection_bounds, ui::style::font_face::icons,
				             ui::style::text_style::single_line_center,
				             projected ? foreground : ui::color(dc.colors.foreground, alpha * 0.45f), {});
			}

			dc.draw_text(icon_to_utf8(icon_index::more), options_bounds, ui::style::font_face::icons,
			             ui::style::text_style::single_line_center, foreground, {});
		}

		if (thumb_alpha > 0.0f)
		{
			dc.draw_rect(control_bounds, ui::color(ui::style::color::group_background, thumb_alpha));
			dc.draw_texture(zoom_texture, dst_quad, zoom_texture->dimensions(), thumb_alpha, sampler);
			dc.draw_border(shown_bounds, zoom_bounds, excluded, excluded);
		}
	}
	catch (const std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}
}

void photo_control::render_zoom_thumb(ui::draw_context& dc, const pointi element_offset) const
{
	render_zoom_overlay(dc, _display, _display->_selected_texture1, bounds, element_offset,
	                    _zoom_navigator_bounds, _zoom_fit_bounds, _zoom_out_bounds, _zoom_100_bounds,
	                    _zoom_in_bounds, _zoom_options_bounds, _zoom_projection_bounds);
}

void side_by_side_control::render_zoom_thumb(ui::draw_context& dc, const pointi element_offset) const
{
	const auto texture = _display->active_zoom_pane() == df::zoom_pane::primary
		                     ? _display->_selected_texture1
		                     : _display->_selected_texture2;
	render_zoom_overlay(dc, _display, texture, bounds, element_offset, _zoom_navigator_bounds,
	                    _zoom_fit_bounds, _zoom_out_bounds, _zoom_100_bounds, _zoom_in_bounds,
	                    _zoom_options_bounds, _zoom_projection_bounds);
}

class zoom_command_controller final : public view_controller
{
	commands _command;
	bool _tracking = false;

public:
	zoom_command_controller(const view_host_ptr& host, const recti bounds, const commands command) :
		view_controller(host, bounds), _command(command)
	{
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::link;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_tracking = true;
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		const auto invoke = _tracking && _bounds.contains(loc);
		_tracking = false;
		if (invoke) _host->invoke(_command);
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		return true;
	}
};

class zoom_options_controller final : public view_controller
{
	view_state& _state;
	bool _tracking = false;

public:
	zoom_options_controller(const view_host_ptr& host, view_state& state, const recti bounds) :
		view_controller(host, bounds), _state(state)
	{
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::link;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_tracking = true;
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		const auto show = _tracking && _bounds.contains(loc);
		_tracking = false;
		if (!show) return;
		const auto command = _state.find_command(commands::menu_zoom);
		const auto menu = command && command->menu ? command->menu() : std::vector<ui::command_ptr>{};
		if (!menu.empty()) _host->track_menu(_bounds, menu);
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		return true;
	}
};

// zoom.md L6: switching a declared panorama between the sphere it describes and the flat pixels it
// stores. One click, and the control it lives on shows which is in force.
class panorama_projection_controller final : public view_controller
{
	display_state_ptr _display;
	bool _tracking = false;

public:
	panorama_projection_controller(const view_host_ptr& host, display_state_ptr display, const recti bounds) :
		view_controller(host, bounds), _display(std::move(display))
	{
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::link;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_tracking = true;
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		const auto invoke = _tracking && _bounds.contains(loc);
		_tracking = false;
		if (invoke) _display->toggle_panorama_projection();
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		return true;
	}
};

class zoom_navigator_controller final : public view_controller
{
	display_state_ptr _display;
	bool _tracking = false;

public:
	zoom_navigator_controller(const view_host_ptr& host, display_state_ptr display, const recti bounds) :
		view_controller(host, bounds), _display(std::move(display))
	{
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::size_all;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_tracking = true;
		move_to(loc);
	}

	void on_mouse_move(const pointi loc) override
	{
		if (_tracking) move_to(loc);
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		if (_tracking) move_to(loc);
		_tracking = false;
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		return true;
	}

	void move_to(const pointi loc) const
	{
		const auto local = pointd(loc - _bounds.top_left());
		_display->zoom_center(df::zoom_view_state::navigator_center(local, sized(_bounds.extent())));
	}
};

class comparison_zoom_controller final : public view_controller
{
	display_state_ptr _display;
	df::zoom_pane _pane;
	df::zoom_view_state _start_zoom_state;
	bool _tracking = false;
	bool _region_select = false;
	bool _inspect_active = false;
	bool _pan_active = false;
	bool _committed = false;

public:
	comparison_zoom_controller(const view_host_ptr& host, display_state_ptr display, const df::zoom_pane pane,
	                           const recti pane_bounds) :
		view_controller(host, pane_bounds), _display(std::move(display)), _pane(pane)
	{
		_display->active_zoom_pane(_pane);
		_start_zoom_state = _display->zoom_state();
	}

	~comparison_zoom_controller() override
	{
		if (_tracking && !_committed) _display->restore_zoom_state(_start_zoom_state);
	}

	ui::style::cursor cursor() const override
	{
		if (_region_select) return ui::style::cursor::select;
		if (!_tracking && ui::current_key_state().control) return ui::style::cursor::select;
		return _display->zoom() || _inspect_active ? ui::style::cursor::size_all : ui::style::cursor::zoom;
	}

	void draw(ui::draw_context& dc) override
	{
		if (_region_select && _tracking)
		{
			const auto selection = recti(_start_loc, _last_loc).normalise().crop(_bounds);
			dc.draw_rect(selection, ui::color(ui::style::color::dialog_selected_background, 0.5));
		}
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_display->active_zoom_pane(_pane);
		_start_zoom_state = _display->zoom_state();
		_tracking = true;
		_last_loc = _start_loc = loc;
		_region_select = keys.control;
		_inspect_active = false;
		_pan_active = false;
		_committed = false;
		if (_region_select) return;
		if (_display->zoom())
		{
			_pan_active = true;
		}
		else
		{
			_display->inspect_at_100(pointd(loc - _bounds.top_left()));
			_inspect_active = true;
		}
	}

	void on_mouse_move(const pointi loc) override
	{
		if (!_tracking) return;
		_last_loc = loc;
		if (_region_select) return;
		if (_pan_active)
		{
			const auto delta = pointd(loc - _start_loc);
			const auto ramp = 120.0 * _host->owner()->scale_factor();
			_display->pan_zoom(df::zoom_view_state::accelerate_pan(delta, ramp), _start_zoom_state);
		}
		else if (_inspect_active)
		{
			const auto local = loc - _bounds.top_left();
			const auto extent = _bounds.extent();
			_display->zoom_center({
				std::clamp(local.x / static_cast<double>(std::max(1, extent.cx)), 0.0, 1.0),
				std::clamp(local.y / static_cast<double>(std::max(1, extent.cy)), 0.0, 1.0)
			});
		}
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		_tracking = false;
		if (_region_select)
		{
			const auto selection = recti(_start_loc, loc).normalise().crop(_bounds);
			_region_select = false;
			if (selection.width() >= 8 && selection.height() >= 8)
				_display->zoom_region(rectd(selection.offset(-_bounds.top_left())));
		}
		else if (_inspect_active && !_committed)
		{
			_display->restore_zoom_state(_start_zoom_state);
		}
		_inspect_active = false;
		_pan_active = false;
		_committed = false;
	}

	bool key_down(const char32_t key, const ui::key_state keys) override
	{
		if (_tracking && _inspect_active && key == keys::SPACE)
		{
			_display->commit_inspect();
			_committed = true;
			return true;
		}
		return false;
	}

	bool escape() override
	{
		if (!_tracking && !_region_select && !_inspect_active && !_pan_active) return false;
		if (_tracking) _display->restore_zoom_state(_start_zoom_state);
		_tracking = false;
		_region_select = false;
		_inspect_active = false;
		_pan_active = false;
		return true;
	}
};

class comparison_pane_controller final : public view_controller
{
	display_state_ptr _display;
	df::zoom_pane _pane;
	bool _tracking = false;

public:
	comparison_pane_controller(const view_host_ptr& host, display_state_ptr display, const df::zoom_pane pane,
	                           const recti bounds) :
		view_controller(host, bounds), _display(std::move(display)), _pane(pane)
	{
	}

	ui::style::cursor cursor() const override { return ui::style::cursor::link; }

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override { _tracking = true; }

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		const auto invoke = _tracking && _bounds.contains(loc);
		_tracking = false;
		if (invoke) _display->active_zoom_pane(_pane);
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		return true;
	}
};

// design.md: the region drawn on the displayed picture. Dragging inside it moves it; the three
// buttons close it, zoom to it, and open Edit with it as the crop. The rectangle itself is held on
// the photo_control in source space - this only translates a drag into that space.
class region_controller final : public view_controller
{
	std::shared_ptr<photo_control> _parent;
	pointi _element_offset;
	rectd _start_region;
	bool _tracking = false;
	bool _moving = false;

public:
	region_controller(const view_host_ptr& host, std::shared_ptr<photo_control> parent,
	                  const recti interaction_bounds, const pointi element_offset, const bool moving) :
		view_controller(host, interaction_bounds), _parent(std::move(parent)), _element_offset(element_offset),
		_start_region(_parent->region()), _moving(moving)
	{
	}

	~region_controller() override
	{
		// An interrupted drag must put back what it took: a Ctrl-draw cleared the rectangle that was
		// there, so dropping the flag alone would destroy it.
		if (_tracking)
		{
			if (_moving) _parent->release_region_drag();
			else _parent->cancel_region_drag();
		}
	}

	ui::style::cursor cursor() const override
	{
		if (_tracking) return _moving ? ui::style::cursor::size_all : ui::style::cursor::select;
		// Ctrl inside the rectangle draws a replacement rather than moving it, so the cursor has to say
		// which of the two the press will get.
		return ui::current_key_state().control ? ui::style::cursor::select : ui::style::cursor::size_all;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_tracking = true;
		_last_loc = _start_loc = loc;
		_start_region = _parent->region();

		// design.md: a drag replaces whatever was there. Ctrl at the moment of the press means draw,
		// even inside the rectangle that is already here, so a smaller region does not first need the
		// current one cleared.
		_moving = !keys.control;
		if (_moving) _parent->begin_region_move();
		else _parent->begin_region_drag();
	}

	void on_mouse_move(const pointi loc) override
	{
		if (!_tracking) return;
		_last_loc = loc;

		const auto image = _parent->image_bounds(_element_offset);
		const auto source = _parent->source_extent();

		if (_moving)
		{
			const auto delta = pointd(loc - _start_loc);
			const auto scale_x = image.Width > 0.0 ? source.Width / image.Width : 0.0;
			const auto scale_y = image.Height > 0.0 ? source.Height / image.Height : 0.0;
			_parent->region(df::offset_source_rect(_start_region, {delta.X * scale_x, delta.Y * scale_y}, source));
			return;
		}

		_parent->region(df::client_rect_to_source(rectd(recti(_start_loc, loc)), image, source));
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		on_mouse_move(loc);
		_tracking = false;
		_parent->release_region_drag();

		// A rectangle too small to see is a click that happened to move, not a region. A Ctrl-draw
		// cleared what was there when it began, so this puts it back rather than committing the empty
		// rectangle - the same answer escape() and the destructor already give, and without it a stray
		// Ctrl-click inside an existing region silently deleted it.
		const auto drawn = _parent->region_bounds(_element_offset);
		if (!_moving && (drawn.width() < 8 || drawn.height() < 8)) _parent->region(_start_region);

		_host->invalidate_view(view_invalid::view_redraw | view_invalid::controller);
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		_parent->release_region_drag();
		_parent->region(_start_region);
		return true;
	}
};
// One of the region's three buttons. `commands::none` is close, which belongs to the region rather
// than to the application and so has no command of its own.
class region_command_controller final : public view_controller
{
	std::shared_ptr<photo_control> _parent;
	view_state& _state;
	pointi _element_offset;
	commands _command;
	bool _tracking = false;

public:
	region_command_controller(const view_host_ptr& host, std::shared_ptr<photo_control> parent, view_state& state,
	                          const recti bounds, const pointi element_offset, const commands command) :
		view_controller(host, bounds), _parent(std::move(parent)), _state(state), _element_offset(element_offset),
		_command(command)
	{
	}

	ui::style::cursor cursor() const override
	{
		return ui::style::cursor::link;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		_tracking = true;
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		const auto invoke = _tracking && _bounds.contains(loc);
		_tracking = false;
		if (!invoke) return;

		if (_command == commands::none)
		{
			_parent->clear_region();
			return;
		}

		if (_command == commands::view_zoom)
		{
			// The same region-to-viewport primitive the transient Ctrl-drag used, in the viewport
			// coordinates the zoom model takes.
			const auto region = _parent->region_bounds(_element_offset);
			const auto viewport = _parent->bounds.offset(_element_offset);
			_parent->clear_region();
			if (!region.is_empty()) _parent->_display->zoom_region(rectd(region.offset(-viewport.top_left())));
			return;
		}

		// An unresolved command is dropped rather than invoked, for the same reason the items menu
		// drops one: asking the host to perform a command the state cannot describe is not something
		// the button can promise.
		const auto command = _state.find_command(_command);
		if (!command || !command->enable) return;

		const auto item = _parent->_display->_item1;
		if (!item) return;

		// After the item, because source_extent() reads the display the line above just tested.
		const auto source = _parent->source_extent();
		if (source.Width <= 0.0 || source.Height <= 0.0) return;

		// Handed on as fractions of the displayed picture: what is on screen may be a stand-in, and
		// Edit reads the file for itself, so pixel counts taken here would not be the same grid.
		const auto drawn = _parent->region();
		const rectd normalised{
			drawn.X / source.Width, drawn.Y / source.Height,
			drawn.Width / source.Width, drawn.Height / source.Height
		};

		_state._pending_edit_crop = view_state::pending_edit_crop_t{item->path(), normalised};
		_parent->clear_region();
		_host->invoke(_command);
	}

	bool escape() override
	{
		if (!_tracking) return false;
		_tracking = false;
		return true;
	}
};

class pan_controller final : public view_controller, public std::enable_shared_from_this<pan_controller>
{
public:
	std::shared_ptr<photo_control> _parent;
	view_state& _state;
	pointi _element_offset;
	df::zoom_view_state _start_zoom_state;
	// zoom.md: a projected panorama is looked around by grabbing it, so the gesture carries the
	// camera it started from and is recomputed from the drag origin on every move.
	panorama_view _start_panorama_view;
	bool _looking = false;
	bool _tracking = false;
	bool _drawing_region = false;
	bool _auto_pan = false;
	pointd _auto_velocity;
	pointd _auto_offset;
	double _auto_last_time = 0.0;

	pan_controller(const view_host_ptr& host, std::shared_ptr<photo_control> parent, view_state& s,
	               const recti bounds, const pointi element_offset) : view_controller(host, bounds),
	                                                                  _parent(std::move(parent)), _state(s),
	                                                                  _element_offset(element_offset)
	{
	}

	~pan_controller() override
	{
		ui::animations.erase(this);
		// An interrupted drag would otherwise leave _region_dragging set, and the region's buttons are
		// hidden while it is - permanently, since nothing else clears it.
		if (_drawing_region) _parent->cancel_region_drag();
		else if (_tracking && _looking) _parent->_display->restore_panorama_view(_start_panorama_view);
		else if (_tracking) _parent->_display->restore_zoom_state(_start_zoom_state);
	}

	void draw(ui::draw_context& rc) override
	{
	}

	ui::style::cursor cursor() const override
	{
		if (_drawing_region) return ui::style::cursor::select;
		if (!_tracking && !_auto_pan && ui::current_key_state().control) return ui::style::cursor::select;
		// Grabbing the sphere is a different gesture from pushing a rectangle around, and says so.
		if (_looking || (!_tracking && _parent->_display->is_panorama_projected()))
			return _looking ? ui::style::cursor::hand_down : ui::style::cursor::hand_up;
		return ui::style::cursor::size_all;
	}

	void on_mouse_left_button_down(const pointi loc, const ui::key_state keys) override
	{
		if (_auto_pan)
		{
			stop_auto_pan();
			return;
		}
		_start_zoom_state = _parent->_display->zoom_state();
		_start_panorama_view = _parent->_display->panorama().view;
		_looking = _parent->_display->is_panorama_projected();
		_last_loc = _start_loc = loc;
		_tracking = true;

		// design.md: Ctrl and drag draws a region on the picture, at whatever scale it is being shown.
		// Not over a projection: the rectangle would be drawn in sphere space and stored as source
		// pixels, which is a crop of somewhere else.
		_drawing_region = keys.control && !_looking;
		if (_drawing_region)
		{
			_parent->begin_region_drag();
			return;
		}

		if (_looking) return;

		scroll_to(loc);
	}

	void on_mouse_move(const pointi loc) override
	{
		if (_auto_pan)
		{
			_auto_velocity = df::zoom_view_state::auto_pan_velocity(pointd(loc - _start_loc),
			                                                        12.0 * _host->owner()->scale_factor());
			return;
		}
		if (_tracking)
		{
			if (_drawing_region)
			{
				_last_loc = loc;
				_parent->drag_region(_start_loc, loc, _element_offset);
				return;
			}
			if (_looking)
			{
				_last_loc = loc;
				_parent->_display->panorama_drag(pointd(loc - _start_loc), _start_panorama_view);
				return;
			}
			scroll_to(loc);
			return;
		}

		// zoom.md: looking around a panorama. No button is held, so this neither begins nor ends a
		// drag, and press-and-hold inspect zoom is untouched.
		if (_parent->_display->is_looking_around())
		{
			_parent->_display->look_around_at(pointd(loc - _parent->bounds.offset(_element_offset).top_left()));
		}
	}

	void on_mouse_left_button_up(const pointi loc, const ui::key_state keys) override
	{
		_tracking = false;
		_looking = false;
		if (_drawing_region)
		{
			_drawing_region = false;
			_parent->end_region_drag(_start_loc, loc, _element_offset);
		}
	}

	void on_mouse_middle_button_down(const pointi loc, const ui::key_state keys) override
	{
		if (_auto_pan)
		{
			stop_auto_pan();
			return;
		}

		// Auto-pan moves the flat centre, which a projection is not showing. Left running it would
		// repaint at full rate for nothing and leave the picture jumped when the user returns to it.
		if (_parent->_display->is_panorama_projected()) return;

		_start_zoom_state = _parent->_display->zoom_state();
		_start_loc = loc;
		_auto_velocity = {};
		_auto_offset = {};
		_auto_last_time = df::now();
		_auto_pan = true;
		const auto weak = weak_from_this();
		ui::animations[this] = [weak]
		{
			const auto controller = weak.lock();
			if (!controller || !controller->_auto_pan) return false;
			const auto now = df::now();
			const auto elapsed = std::clamp(now - controller->_auto_last_time, 0.0, 0.05);
			controller->_auto_last_time = now;
			controller->_auto_offset = controller->_auto_offset + controller->_auto_velocity * elapsed;
			controller->_parent->_display->pan_zoom(controller->_auto_offset, controller->_start_zoom_state);
			controller->_host->invalidate_view(view_invalid::view_redraw);
			return true;
		};
		_host->invalidate_view(view_invalid::animations);
	}

	bool escape() override
	{
		if (_auto_pan)
		{
			stop_auto_pan();
			return true;
		}
		if (_tracking)
		{
			_tracking = false;
			if (_drawing_region)
			{
				_drawing_region = false;
				_parent->cancel_region_drag();
			}
			else if (_looking)
			{
				_looking = false;
				_parent->_display->restore_panorama_view(_start_panorama_view);
			}
			else
			{
				_parent->_display->restore_zoom_state(_start_zoom_state);
			}
			return true;
		}
		return false;
	}

	void stop_auto_pan()
	{
		_auto_pan = false;
		_auto_velocity = {};
		ui::animations.erase(this);
		_host->invalidate_view(view_invalid::controller | view_invalid::view_redraw);
	}

	void scroll_to(const pointi loc)
	{
		if (_last_loc != loc)
		{
			const auto delta = pointd(loc - _start_loc);
			const auto ramp = 120.0 * _host->owner()->scale_factor();
			const auto offset = df::zoom_view_state::accelerate_pan(delta, ramp);
			_parent->_display->pan_zoom(pointd(offset), _start_zoom_state);
			_last_loc = loc;
		}
	}
};

view_controller_ptr view_elements::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                            const pointi element_offset,
                                                            hit_test_context& ctx)
{
	view_controller_ptr result;

	if (bounds.contains(loc - element_offset))
	{
		for (const auto& c : _children)
		{
			if (!c->is_visible()) continue;
			result = c->controller_from_location(host, loc, element_offset, ctx);
			if (result) break;
		}
	}

	return result;
}


view_controller_ptr photo_control::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                            const pointi element_offset,
                                                            hit_test_context& ctx)
{
	const auto logical_loc = loc + element_offset;
	const auto image_bounds = _display->_selected_texture1
		                          ? _display->_selected_texture1->display_bounds().offset(element_offset)
		                          : recti{};
	const auto durable_zoom = _display->zoom() && !_display->is_temporary_zoom();

	// design.md: the region is a layer over the picture, not over the window. The zoom chrome keeps
	// its precedence over all of it - including the region's own buttons, which are clamped into the
	// element and so can land on the navigator - because a rectangle a user happened to draw must not
	// make the tools it covers unclickable. Testing the chrome first is what states that precedence,
	// and recording each rect in the same breath is what keeps a tool reachable from inside the
	// rectangle or the picture that covers it.
	if (durable_zoom)
	{
		if (_zoom_grading_element && ctx.occluded(_zoom_grading_element->bounds.offset(element_offset)))
		{
			return _zoom_grading_element->controller_from_location(host, loc, element_offset, ctx);
		}

		if (ctx.occluded(_zoom_options_bounds))
		{
			return std::make_shared<zoom_options_controller>(host, _state, _zoom_options_bounds);
		}

		if (ctx.occluded(_zoom_projection_bounds))
		{
			return std::make_shared<panorama_projection_controller>(host, _display, _zoom_projection_bounds);
		}

		const std::array zoom_commands{
			std::pair{_zoom_fit_bounds, commands::view_zoom_fit},
			std::pair{_zoom_out_bounds, commands::view_zoom_out},
			std::pair{_zoom_100_bounds, commands::view_zoom_100},
			std::pair{_zoom_in_bounds, commands::view_zoom_in}
		};

		for (const auto& [command_bounds, command] : zoom_commands)
		{
			if (ctx.occluded(command_bounds))
			{
				return std::make_shared<zoom_command_controller>(host, command_bounds, command);
			}
		}

		if (ctx.occluded(_zoom_navigator_bounds))
		{
			return std::make_shared<zoom_navigator_controller>(host, _display, _zoom_navigator_bounds);
		}
	}

	if (has_region())
	{
		const std::array region_commands{
			std::pair{_region_close_bounds, commands::none},
			std::pair{_region_zoom_bounds, commands::view_zoom},
			std::pair{_region_crop_bounds, commands::tool_edit}
		};

		for (const auto& [command_bounds, command] : region_commands)
		{
			if (ctx.occluded(command_bounds))
			{
				return std::make_shared<region_command_controller>(host, shared_from_this(), _state, command_bounds,
				                                                   element_offset, command);
			}
		}

		// Dragging inside the rectangle moves it. Drawing a new one is Ctrl at the moment of the press,
		// which region_controller reads for itself.
		if (const auto region = region_bounds(element_offset); ctx.occluded(region))
		{
			return std::make_shared<region_controller>(host, shared_from_this(), region, element_offset, true);
		}
	}

	if (!_display->zoom() && !image_bounds.is_empty() && image_bounds.contains(loc) &&
		_display->can_zoom())
	{
		const auto view_bounds = bounds.offset(element_offset);
		return std::make_shared<zoom_controller>(host, shared_from_this(), _state, image_bounds, view_bounds,
		                                         element_offset);
	}

	if (bounds.contains(logical_loc) && _can_pan && _display->zoom())
	{
		return std::make_shared<pan_controller>(host, shared_from_this(), _state, bounds.offset(element_offset),
		                                        element_offset);
	}

	return nullptr;
}

view_controller_ptr view_element::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                           const pointi element_offset,
                                                           hit_test_context& ctx)
{
	return nullptr;
}


static ui::color calc_clr(const view_element_style& style)
{
	auto result_clr = ui::color();

	const auto tracking = style && view_element_style::tracking;
	const auto hover = style && view_element_style::hover;
	const auto selected = style && view_element_style::selected;
	const auto highlight = style && view_element_style::highlight;

	if (tracking || hover || selected || highlight)
	{
		result_clr = view_handle_color(selected || highlight, hover, tracking, true, true);
		result_clr.a = 1.0f;
	}
	else if (style && view_element_style::checked)
	{
		result_clr.a = 0.22f;
	}
	else if (style && view_element_style::important)
	{
		result_clr = ui::color(ui::style::color::important_background);
		result_clr.a = 1.0f;
	}
	else if (style && view_element_style::info)
	{
		result_clr = ui::color(ui::style::color::info_background);
		result_clr.a = 1.0f;
	}
	else if (style && view_element_style::dark_background)
	{
		result_clr.a = 0.777f;
	}
	else if (style && view_element_style::shaded_background)
	{
		result_clr.a = 0.44f;
	}
	else if (style && view_element_style::background)
	{
		result_clr.a = 0.222f;
	}

	return result_clr;
}

void view_element::update_background_color()
{
	_bg_target = _bg_color = calc_clr(style);
}

void view_element::set_style_bit(const view_element_style mask, const bool state)
{
	auto new_style = style;

	if (state)
	{
		new_style |= mask;
	}
	else
	{
		new_style &= ~mask;
	}

	if (new_style != style)
	{
		style = new_style;
		update_background_color();
	}
}


void view_element::set_style_bit(const view_element_style mask, const bool state, const view_host_base_ptr& view,
                                 const view_element_ptr& e)
{
	auto new_style = style;

	if (state)
	{
		new_style |= mask;
	}
	else
	{
		new_style &= ~mask;
	}

	if (new_style != style)
	{
		style = new_style;
		const auto bg = calc_clr(style);

		if (view)
		{
			if (_bg_target != bg && e)
			{
				if (ui::is_alpha_zero(_bg_color.a))
				{
					_bg_color.r = bg.r;
					_bg_color.g = bg.g;
					_bg_color.b = bg.b;
					_bg_target = bg;
				}
				else if (ui::is_alpha_zero(bg.a))
				{
					_bg_target.a = 0.0f;
				}
				else
				{
					_bg_target = bg;
				}

				ui::animations[this] = [view, e]
				{
					const auto dd = e->_bg_target - e->_bg_color;
					bool invalidate = false;

					if (dd.abs_sum() > ui::color::color_epsilon)
					{
						// Gated and timed as animate_alpha is: under the CPU renderer the colour is
						// taken at once, and elsewhere the fade lasts as long at any refresh rate.
						e->_bg_color += ui::animations_enabled ? dd * ui::animation_step_factor : dd;
						view->invalidate_element(e);
						invalidate = true;
					}

					return invalidate;
				};

				view->invalidate_view(view_invalid::animations);
			}
		}
		else
		{
			_bg_target = _bg_color = calc_clr(style);
		}
	}
}

view_controller_ptr video_control::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                            const pointi element_offset,
                                                            hit_test_context& ctx)
{
	return default_controller_from_location(*this, host, loc, element_offset, ctx);
}

view_controller_ptr audio_control::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                            const pointi element_offset,
                                                            hit_test_context& ctx)
{
	return default_controller_from_location(*this, host, loc, element_offset, ctx);
}

view_controller_ptr side_by_side_control::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                                   const pointi element_offset,
                                                                   hit_test_context& ctx)
{
	const auto durable_zoom = _display->zoom() && !_display->is_temporary_zoom();
	if (durable_zoom)
	{
		const auto index = _display->active_zoom_pane() == df::zoom_pane::primary ? 0u : 1u;
		const auto& grading = _zoom_grading_elements[index];
		if (grading && ctx.occluded(grading->bounds.offset(element_offset)))
		{
			return grading->controller_from_location(host, loc, element_offset, ctx);
		}
		if (ctx.occluded(_zoom_options_bounds))
		{
			return std::make_shared<zoom_options_controller>(host, _state, _zoom_options_bounds);
		}
		const std::array zoom_commands{
			std::pair{_zoom_fit_bounds, commands::view_zoom_fit},
			std::pair{_zoom_out_bounds, commands::view_zoom_out},
			std::pair{_zoom_100_bounds, commands::view_zoom_100},
			std::pair{_zoom_in_bounds, commands::view_zoom_in}
		};
		for (const auto& [command_bounds, command] : zoom_commands)
		{
			if (ctx.occluded(command_bounds))
			{
				return std::make_shared<zoom_command_controller>(host, command_bounds, command);
			}
		}
		if (ctx.occluded(_zoom_navigator_bounds))
		{
			return std::make_shared<zoom_navigator_controller>(host, _display, _zoom_navigator_bounds);
		}
	}

	const auto video_control_bounds = _display->_compare_video_control_bounds.offset(element_offset);
	if (_display->_is_compare_video && ctx.occluded(video_control_bounds))
	{
		return std::make_shared<preview_controller>(host, shared_from_this(), video_control_bounds,
		                                            _display->_compare_video_scrubber_bounds.offset(element_offset));
	}

	for (auto i = 0u; i < _display->_pane_marker_bounds.size(); ++i)
	{
		const auto marker = _display->_pane_marker_bounds[i];
		if (!ctx.occluded(marker)) continue;

		// Magnified there is only one marker and it names the pane you are already in, so it flips.
		const auto pane = _display->is_zoom_mode()
			                  ? df::comparison_zoom_state::other(_display->active_zoom_pane())
			                  : i == 0
			                  ? df::zoom_pane::primary
			                  : df::zoom_pane::secondary;
		return std::make_shared<comparison_pane_controller>(host, _display, pane, marker);
	}

	const auto compare_bounds = comparison_divider_device_bounds(_display->_compare_bounds, element_offset);
	if (!_display->is_zoom_mode() && _display->_can_compare && compare_bounds.contains(loc))
	{
		return std::make_shared<compare_controller>(host, shared_from_this(), compare_bounds);
	}

	if (!_display->_comparing && _display->can_zoom())
	{
		auto pane_bounds = bounds.offset(element_offset);
		const auto zoomed = _display->is_zoom_mode();
		const auto pane = zoomed
			                  ? _display->active_zoom_pane()
			                  : loc.x < pane_bounds.center().x
			                  ? df::zoom_pane::primary
			                  : df::zoom_pane::secondary;
		if (!zoomed)
		{
			if (pane == df::zoom_pane::primary) pane_bounds.right = pane_bounds.center().x;
			else pane_bounds.left = pane_bounds.center().x;
		}
		if (pane_bounds.contains(loc))
		{
			return std::make_shared<comparison_zoom_controller>(host, _display, pane, pane_bounds);
		}
	}

	ctx.passed_over(compare_bounds);
	return default_controller_from_location(*this, host, loc, element_offset, ctx);
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void link_element::update_check(const view_element_event& event)
{
	const bool is_checked = _cmd != commands::none && event.host->is_command_checked(_cmd);

	if (is_checked != is_style_bit_set(view_element_style::checked))
	{
		set_style_bit(view_element_style::checked, is_checked);
		event.host->invalidate_view(view_invalid::view_redraw);
	}
}

void link_element::dispatch_event(const view_element_event& event)
{
	if (event.type == view_element_event_type::update_command_state)
	{
		update_check(event);
	}
	else if (event.type == view_element_event_type::invoke)
	{
		if (_cmd != commands::none)
		{
			event.host->invoke(_cmd);
			update_check(event);
		}
		else if (_invoke)
		{
			_invoke();
		}
	}
	else
	{
		text_element_base::dispatch_event(event);
	}
}

void link_element::tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const
{
	if (_tooltip)
	{
		_tooltip(hover);
	}
	else if (_cmd != commands::none)
	{
		hover.id = _cmd;
	}

	hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
}

view_controller_ptr link_element::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                           const pointi element_offset,
                                                           hit_test_context& ctx)
{
	return default_controller_from_location(*this, host, loc, element_offset, ctx);
}

namespace
{
	static_assert(std::size(rate_label_defs) == rate_label_control::entry_count);

	// Row order matches the sidebar filters and the keyboard keys: Reject, 6, 7, 8, 9, P.
	constexpr commands rate_label_commands[] = {
		commands::rate_rejected,
		commands::label_select,
		commands::label_second,
		commands::label_approved,
		commands::label_review,
		commands::label_to_do,
	};

	static_assert(std::size(rate_label_commands) == rate_label_control::entry_count);

	bool is_entry_set(const df::item_element_ptr& i, const rate_label_def& e)
	{
		return e.key.empty() ? i->rating() == -1 : str::icmp(i->label(), e.key) == 0;
	}

	std::string entry_text(const rate_label_def& e)
	{
		return e.key.empty() ? std::string(tt.command_rate_rejected) : prop::format_label(e.key);
	}
}

void rating_control::tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const
{
	hover.elements->add(make_icon_element(icon_index::star, flex_item::no_break));

	const auto current_rating = displayed_rating();
	const auto is_rejected = current_rating == -1;
	const auto has_stars = current_rating >= 1;

	if (_can_edit)
	{
		if (has_stars && current_rating == last_hover_rating)
		{
			hover.elements->add(std::make_shared<text_element>(str_format(tt.rating_remove_fmt.sv(), current_rating)));
		}
		else
		{
			hover.elements->
			      add(std::make_shared<text_element>(format_plural_text(tt.rating_set_fmt, last_hover_rating)));

			if (has_stars)
			{
				hover.elements->add(
					std::make_shared<text_element>(str_format(tt.rating_remove_fmt.sv(), current_rating)));
			}
		}
	}

	const auto table = std::make_shared<ui::table_element>();
	table->add(std::format("{}:", tt.prop_name_label), prop::format_label(_item->label()));
	table->add(std::format("{}:", tt.prop_name_rating), prop::format_rating(current_rating));
	hover.elements->add(table);

	if (_can_edit && is_rejected)
	{
		hover.elements->add(std::make_shared<text_element>(tt.rating_replaces_reject, ui::style::font_face::dialog,
		                                                   ui::style::text_style::multiline, flex_item::line_break));
	}

	auto rating_bounds = bounds.offset(element_offset);
	rating_bounds.right = rating_bounds.left + std::max(1, last_hover_rating) * _icon_cxy;
	rating_bounds.left = rating_bounds.right - _icon_cxy;

	hover.window_bounds = rating_bounds;
	hover.active_bounds = rating_bounds;

	if (_can_edit && show_accelerator)
	{
		hover.elements->add(std::make_shared<action_element>(tt.rating_keys));
	}

	if (!_can_edit)
	{
		const df::item_set items = {{_item}};
		const auto can_process = items.can_process(df::process_items_type::can_save_metadata, false, {});
		hover.elements->add(std::make_shared<text_element>(can_process.to_string(), ui::style::font_face::dialog,
		                                                   ui::style::text_style::multiline,
		                                                   flex_item::line_break));
	}
}

// The bubble names the value the click under the pointer would store, because the batch field shows
// no item state to infer it from.
void rating_edit_control::tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const
{
	const auto rating = cell_to_rating(std::max(0, _hover_cell));

	hover.elements->add(make_icon_element(rating < 0 ? rate_label_reject.icon : icon_index::star,
	                                      flex_item::no_break));

	if (rating < 0)
	{
		hover.elements->add(std::make_shared<text_element>(tt.command_rate_rejected));
	}
	else if (_val == rating)
	{
		hover.elements->add(std::make_shared<text_element>(str_format(tt.rating_remove_fmt.sv(), rating)));
	}
	else
	{
		hover.elements->add(std::make_shared<text_element>(format_plural_text(tt.rating_set_fmt, rating)));
	}

	hover.window_bounds = hover.active_bounds = bounds.offset(element_offset);
}

void rate_label_control::render(ui::draw_context& dc, const pointi element_offset) const
{
	const auto i = _item;
	if (!i) return;

	render_background(dc, element_offset);

	const auto view_bounds = bounds.offset(element_offset);
	_view_bounds = view_bounds;
	const auto alpha = _can_edit ? dc.colors.alpha : dc.colors.alpha * 0.25f;

	draw_rate_label_badge(dc, i->label(), i->rating(), view_bounds, alpha);
}

static ui::command_ptr def_menu_command(const icon_index icon, const std::string_view text, const ui::color32 clr,
                                        std::function<void()> invoke, const bool is_checked,
                                        const bool is_enabled = true)
{
	auto c = std::make_shared<ui::command>();
	c->icon = icon;
	c->text = text;
	c->clr = clr;
	c->invoke = std::move(invoke);
	c->checked = is_checked;
	c->enable = is_enabled;
	return c;
}


// The whole grading vocabulary lives in the menu, so the badge stays one icon wide while every mark
// remains one click away with its own colour, glyph, and accelerator.
void rate_label_control::dispatch_event(const view_element_event& event)
{
	if (event.type == view_element_event_type::invoke && _can_edit)
	{
		std::vector<ui::command_ptr> menu;
		menu.reserve(entry_count);

		for (auto n = 0; n < entry_count; ++n)
		{
			const auto& e = rate_label_defs[n];
			const auto is_set = is_entry_set(_item, e);

			auto apply = [self = shared_from_this(), &e, is_set, host = event.host]
			{
				metadata_edits edits;

				if (e.key.empty())
				{
					edits.rating = self->_item->rating() == -1 ? 0 : -1;
				}
				else
				{
					edits.label = is_set ? std::string_view{} : e.key;
				}

				const df::item_elements items_to_modify = {{self->_item}};
				self->_state.modify_items(host->owner(), e.icon, tt.title_updating, items_to_modify, edits, host);
			};

			auto c = def_menu_command(e.icon, entry_text(e), e.clr, std::move(apply), is_set);

			if (show_accelerator)
			{
				const auto command = _state.find_command(rate_label_commands[n]);
				if (command) c->keyboard_accelerator_text = command->keyboard_accelerator_text;
			}

			menu.emplace_back(std::move(c));
		}

		event.host->track_menu(_view_bounds, menu);
	}
}

// The bubble is the only place the grading rules are stated, so it names the current mark, reports
// both values, and warns about the exclusive value a menu choice would replace.
void rate_label_control::tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const
{
	const auto current_label = _item->label();
	const auto current_rating = _item->rating();
	const auto* const def = find_rate_label_def(current_label);
	const auto icon = current_rating < 0 ? rate_label_reject.icon : def ? def->icon : icon_index::flag;

	hover.elements->add(make_icon_element(icon, flex_item::no_break));
	hover.elements->add(std::make_shared<text_element>(tt.command_view_rate_label, ui::style::font_face::dialog,
	                                                   ui::style::text_style::multiline,
	                                                   flex_item::line_break));

	const auto table = std::make_shared<ui::table_element>();
	table->add(std::format("{}:", tt.prop_name_label), prop::format_label(current_label));
	table->add(std::format("{}:", tt.prop_name_rating), prop::format_rating(current_rating));

	hover.elements->add(table);

	// Only the exclusive values a menu choice would overwrite are worth stating.
	if (_can_edit)
	{
		if (!current_label.empty())
		{
			hover.elements->add(std::make_shared<text_element>(
				str_format(tt.label_replaces_fmt.sv(), prop::format_label(current_label)),
				ui::style::font_face::dialog, ui::style::text_style::multiline, flex_item::line_break));
		}

		if (current_rating > 0)
		{
			hover.elements->add(std::make_shared<text_element>(tt.label_reject_replaces_rating,
			                                                   ui::style::font_face::dialog,
			                                                   ui::style::text_style::multiline,
			                                                   flex_item::line_break));
		}
	}
	else
	{
		const df::item_set items = {{_item}};
		const auto can_edit = items.can_process(df::process_items_type::can_save_metadata, false, {});
		hover.elements->add(std::make_shared<text_element>(can_edit.to_string(), ui::style::font_face::dialog,
		                                                   ui::style::text_style::multiline,
		                                                   flex_item::line_break));
	}

	hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
}

void preview_control::render(ui::draw_context& dc, const pointi element_offset) const
{
	const auto view_bounds = bounds.offset(element_offset);
	_view_bounds = view_bounds;
	auto bg = calc_background_color(dc);

	if (_ts->is_preview_rendering())
	{
		bg.merge(ui::color(ui::style::color::important_background, dc.colors.alpha));
	}

	xdraw_icon(dc, icon_index::preview, view_bounds, ui::color(dc.colors.foreground, dc.colors.alpha), bg);
}

void preview_control::dispatch_event(const view_element_event& event)
{
	if (event.type == view_element_event_type::invoke)
	{
		const auto parent = event.host->owner();

		auto show_raw_always = [ts = _ts]
		{
			setting.raw_preview = false;
			ts->load_raw();
		};

		auto show_raw_preview_always = []
		{
			setting.raw_preview = true;
		};

		auto show_raw_this_only = [ts = _ts]
		{
			ts->load_raw();
		};

		const std::vector<ui::command_ptr> result = {
			def_menu_command(icon_index::none, tt.show_raw, 0, show_raw_always, !setting.raw_preview),
			def_menu_command(icon_index::none, tt.preview_show_preview, 0, show_raw_preview_always,
			                 setting.raw_preview),
			def_menu_command(icon_index::none, tt.show_raw_now, 0, show_raw_this_only, false),
		};

		event.host->track_menu(_view_bounds, result);
	}
}

void preview_control::tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const
{
	hover.elements->add(make_icon_element(icon_index::preview, flex_item::no_break));

	auto text = tt.preview_rendered;
	if (_ts->is_preview()) text = tt.preview_showing;
	if (_ts->is_preview_rendering()) text = tt.preview_rendering;

	hover.elements->add(std::make_shared<text_element>(text, ui::style::font_face::dialog,
	                                                   ui::style::text_style::multiline,
	                                                   flex_item::line_break));
	hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
}

void items_dates_control::tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const
{
	constexpr auto font = ui::style::font_face::dialog;
	const auto md = _item->metadata();

	// The index stores both stamps as UTC instants. Every metadata row above them is a wall clock, so
	// they are converted here or the table lists one instant twice, ten hours apart, under two names -
	// which also defeats the de-duplication shows_own_file_stamp_row exists to do.
	const auto file_created_date = _item->file_created().system_to_local();
	const auto file_modified_date = _item->file_modified().system_to_local();
	const auto created_date = _item->media_created();
	const auto st = created_date.date();
	const auto search = df::search_t().day(st.day, st.month, st.year);

	// Counting the day scans every indexed folder, so it is answered by the query worker. Until it
	// answers, the tooltip shows the dates it already holds and leaves the total out.
	const auto matches = _state.day_item_count(created_date);
	const auto e = hover.elements;

	// Four columns of a table, not a sentence: at the default width the source column wraps its tag
	// name and the row stops reading as one date.
	hover.preferred_size = df::mul_div(view_hover_element::default_preferred_size, 13, 10);

	e->add(make_icon_element(search.first_type()->icon, flex_item::no_break));
	e->add(std::make_shared<text_element>(tt.dates_title, ui::style::font_face::title,
	                                      ui::style::text_style::single_line, flex_item::line_break));

	const auto table = std::make_shared<ui::table_element>(flex_item::center);

	// The source is provenance rather than value, so it is dimmed beside the date it explains.
	const auto source_color = ui::darken(ui::style::color::view_text, 0.22f);

	const auto add_row = [&table, source_color](const std::string_view label, const df::date_t d,
	                                            const std::string_view source)
	{
		auto source_cell = std::make_shared<text_element>(source);
		source_cell->foreground_color(source_color);

		table->add(std::make_shared<text_element>(label),
		           std::make_shared<text_element>(platform::format_date(d)),
		           std::make_shared<text_element>(platform::format_time(d)),
		           source_cell);
	};

	// Naming the tag each date came from is what turns "the date is wrong" into a question with a
	// visible answer, which is the whole difficulty of #184.
	if (md)
	{
		const auto add_date_row = [&add_row, md](const std::string_view label, const prop::date_concept kind)
		{
			const auto d = md->dates.resolve(kind);
			if (!d.is_valid()) return;

			add_row(label, d, prop::date_source_name(md->dates.resolved_source(kind)));
		};

		add_date_row(tt_prep(tt.prop_name_original.sv()), prop::date_concept::original);
		add_date_row(tt_prep(tt.prop_name_created.sv()), prop::date_concept::created);
		add_date_row(tt_prep(tt.prop_name_modified.sv()), prop::date_concept::modified);
	}

	const auto shows_file_created = !md || prop::shows_own_file_stamp_row(
		md->dates, prop::date_source::file_created, file_created_date);
	const auto shows_file_modified = !md || prop::shows_own_file_stamp_row(
		md->dates, prop::date_source::file_modified, file_modified_date);

	if (file_created_date.is_valid() && shows_file_created)
	{
		add_row(tt.dates_file_created, file_created_date, prop::date_source_name(prop::date_source::file_created));
	}

	if (file_modified_date.is_valid() && shows_file_modified)
	{
		add_row(tt.dates_file_modified, file_modified_date, prop::date_source_name(prop::date_source::file_modified));
	}

	e->add(table);

	if (matches)
	{
		e->add(std::make_shared<text_element>(
			str_format(tt.items_created_on_fmt.sv(), *matches, prop::format_date(created_date)), font,
			ui::style::text_style::multiline, flex_item::new_line));
	}

	hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
}

void view_scroller::draw_scroll(ui::draw_context& dc) const
{
	if (can_scroll())
	{
		const auto text_line_height = dc.text_line_height(ui::style::font_face::dialog);
		const auto sb = track_bounds();
		const auto clr = view_handle_color(false, _active, _tracking, dc.frame_has_focus, false,
		                                   ui::color(dc.colors.background));
		const auto bg_clr = ui::color(0x000000, dc.colors.alpha * dc.colors.bg_alpha);
		const auto handle_margin = track_inset();
		const auto left = sb.left + handle_margin;
		const auto right = sb.right - handle_margin;
		// The bands are a fixed map of the list, so they read the same whether or not the pointer
		// is over the column. Only the thumb answers the pointer.
		const auto label_color = ui::color(dc.colors.foreground, dc.colors.alpha);

		// The thumb is the backdrop of the column, not an overlay on it. It grows with the share of
		// the list on screen, so a short list puts it over most of the sections; drawn last it wiped
		// them out, worst of all under the pointer where it is at its brightest.
		dc.draw_rounded_rect(thumb_bounds(), clr, dc.padding1);

		for_each_band([&](const int band_top, const int band_bottom, const view_scroller_section* so)
		{
			const recti rr(left, sb.top + band_top, right, sb.top + band_bottom);

			dc.draw_rounded_rect(rr, bg_clr, dc.padding1);

			if (!so) return;

			// An icon needs less room than a text line, so gate each on what it actually costs.
			if (so->icon != icon_index::none)
			{
				if (band_bottom - band_top > dc.icon_cxy) xdraw_icon(dc, so->icon, rr, label_color, {});
			}
			else if (!so->text.empty() && band_bottom - band_top > text_line_height)
			{
				dc.draw_text(so->text, rr, ui::style::font_face::dialog,
				             ui::style::text_style::single_line_center, label_color, {});
			}
		});
	}
}

view_scroll_anchor view_scroller::capture_anchor(const view_element_ptr& element) const
{
	view_scroll_anchor result;
	result.valid = !_client_bounds.is_empty();
	result.was_at_start = _offset.y == 0;

	const auto max_scroll = std::max(0, _scroll_extent.cy - _client_bounds.height());
	result.scroll_ratio = max_scroll > 0 ? static_cast<double>(_offset.y) / max_scroll : 0.0;

	if (element)
	{
		const auto logical_bounds = _client_bounds.offset(0, _offset.y);
		if (element->bounds.intersects(logical_bounds))
		{
			result.element = element;
			result.device_top = element->bounds.top - _offset.y;
		}
	}

	return result;
}

recti view_scroller::layout_with_footer(const sizei scroll_extent, const recti client_bounds,
                                        recti scroll_bounds, const int footer_extent, const int gap)
{
	recti footer_bounds;
	if (footer_extent > 0 && !scroll_bounds.is_empty())
	{
		const auto height = std::min(footer_extent, scroll_bounds.height());
		footer_bounds = {
			scroll_bounds.left, scroll_bounds.bottom - height,
			scroll_bounds.right, scroll_bounds.bottom
		};
		scroll_bounds.bottom = std::max(scroll_bounds.top, footer_bounds.top - std::max(0, gap));
	}

	layout(scroll_extent, client_bounds, scroll_bounds);
	_footer_bounds = footer_bounds;
	return footer_bounds;
}

int view_scroller::anchor_offset(const view_scroll_anchor& anchor, const bool element_is_current) const
{
	if (!anchor.valid || anchor.was_at_start) return 0;
	if (anchor.element && element_is_current) return anchor.element->bounds.top - anchor.device_top;

	const auto max_scroll = std::max(0, _scroll_extent.cy - _client_bounds.height());
	return df::round(anchor.scroll_ratio * max_scroll);
}

void view_scroller::restore_anchor(const view_host_ptr& host, const view_scroll_anchor& anchor,
                                   const bool element_is_current)
{
	if (!anchor.valid) return;
	scroll_offset(host, _offset.x, anchor_offset(anchor, element_is_current));
}
