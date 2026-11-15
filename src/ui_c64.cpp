// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The Commodore 64 in the media pane: the listing of a disk, a tape or a program file, whose rows run
// what they name; a cartridge, drawn as one with its title on its label; and the machine running either, with
// the way back. The machine itself is c64_session (av_c64.h).

#include "pch.h"
#include "app_text.h"
#include "model.h"
#include "ui_c64.h"
#include "ui_music.h"
#include "ui_view.h"
#include "libdesktop/render/software.h"

namespace
{
	// The C64's own blue and light blue, its screen and its border at power-on.
	constexpr core::color32 c64_blue = core::rgb(33, 27, 174);
	constexpr core::color32 c64_light_blue = core::rgb(95, 83, 254);

	int button_height(render::measure_context& mc)
	{
		return mc.text_line_height(render::font_face::dialog) + mc.padding2 * 2;
	}

	int button_width(render::measure_context& mc, const std::string_view text)
	{
		return mc.measure_text(text, render::font_face::dialog, render::text_style::single_line, 10000).cx +
		       mc.padding2 * 4;
	}

	// A push button in the view's colours: lit under the pointer, and darker while it is held there.
	void draw_button(render::draw_context& dc, const std::string_view text, const recti r, const bool hot,
	                 const bool pressed)
	{
		const auto alpha = dc.colors.alpha;
		auto fill = view_handle_color(false, hot, pressed, true, true);
		fill.a *= alpha;
		dc.draw_rounded_rect(r, fill, dc.padding1);
		dc.draw_text(text, r, render::font_face::dialog, render::text_style::single_line_center,
		             core::color(ui::style::colors.view_text, alpha), {});
	}

	// Start, as a button that plays: the play glyph before its label.
	void draw_start_button(render::draw_context& dc, const recti r, const bool hot, const bool pressed)
	{
		const auto alpha = dc.colors.alpha;
		auto fill = view_handle_color(false, hot, pressed, true, true);
		fill.a *= alpha;
		dc.draw_rounded_rect(r, fill, dc.padding1);

		const auto text = core::color(ui::style::colors.view_text, alpha);
		const auto label =
			dc.measure_text(tt.c64_start, render::font_face::dialog, render::text_style::single_line, r.width()).cx;
		const auto left = r.left + (r.width() - (dc.icon_cxy + dc.padding1 + label)) / 2;
		const recti icon(left, r.top, left + dc.icon_cxy, r.bottom);
		xdraw_icon(dc, icon_index::play, icon, text, {});
		dc.draw_text(tt.c64_start, recti(icon.right + dc.padding1, r.top, r.right, r.bottom), render::font_face::dialog,
		             render::text_style::single_line, text, {});
	}

	int start_button_width(render::measure_context& mc)
	{
		return mc.measure_text(tt.c64_start, render::font_face::dialog, render::text_style::single_line, 10000).cx +
		       mc.icon_cxy + mc.padding1 + mc.padding2 * 4;
	}

	// Commodore's own cartridge shell for the C64 and VIC-20, in millimetres: as wide as its label face and as long
	// as it goes into the port; and the board inside it, whose contacts stand out of the shell's open end.
	constexpr double shell_width = 68.5;
	constexpr double shell_length = 89.0;
	constexpr double board_width = 57.1;

	// It is drawn as the sidebar's calendar draws its blocks: the face true to its shape, and the shell's 20mm
	// receding up and to the right, steeper than it is wide, so its top end and right side show. Both are parts
	// of the face's width, as is how far the board reaches out of the shell, from halfway through it.
	constexpr double depth_x = 0.10;
	constexpr double depth_y = 0.13;
	constexpr double board_reach = 0.12;

	// The drawing's height over its width: the depth, the face and the board below it.
	constexpr double cartridge_aspect = (depth_y + shell_length / shell_width + board_reach - depth_y / 2) /
	                                    (1.0 + depth_x);

	// The cartridge's width in a panel of inner width: as large as a cartridge in the hand reads at, within the
	// panel, and no taller than the stack can spare.
	int cartridge_width(render::measure_context& mc, const int inner)
	{
		const auto least = std::min(inner, core::round(150 * mc.scale_factor));
		return std::clamp(core::mul_div(inner, 9, 20), least, std::max(least, core::round(260 * mc.scale_factor)));
	}

	int cartridge_height(render::measure_context&, const int width)
	{
		return core::round(width * cartridge_aspect);
	}

	// The gap between the case and Start, which the case's shadow falls into.
	int cartridge_shadow(const recti r)
	{
		return std::max(4, r.width() / 28);
	}

	std::string port_label(const int port)
	{
		return str_format(tt.c64_joystick_port_fmt.sv(), port);
	}

	// Where a press began, for an element whose parts are pressed one at a time: the part it began on stays the
	// pressed one until the button is let go, wherever the pointer goes meanwhile.
	template <typename Part>
	bool track_part(const interaction_context& ic, const Part under, const Part none, Part& tracked, Part& hot,
	                bool& pressed)
	{
		if (!ic.tracking) tracked = none;
		else if (tracked == none) tracked = under;

		const auto new_hot = tracked != none ? tracked : under;
		const auto new_pressed = tracked != none && under == tracked;
		if (new_hot == hot && new_pressed == pressed) return false;

		hot = new_hot;
		pressed = new_pressed;
		return true;
	}
}

sizei fit_c64_picture(const sizei avail, bool& whole)
{
	constexpr auto w = c64_session::picture_width;
	constexpr auto h = c64_session::picture_height;
	whole = true;
	if (avail.cx <= 0 || avail.cy <= 0) return {0, 0};

	const auto scale = std::min(avail.cx / static_cast<double>(w), avail.cy / static_cast<double>(h));
	const auto whole_scale = std::floor(scale);

	if (whole_scale >= 1.0 && whole_scale >= scale * 0.8)
	{
		const auto s = static_cast<int>(whole_scale);
		return {w * s, h * s};
	}

	whole = false;
	return {std::max(1, core::round(w * scale)), std::max(1, core::round(h * scale))};
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

commodore_disk_control::commodore_disk_control(view_state& state, display_state_ptr display,
                                               const view_element_options& style_in) :
	view_element(style_in | view_element_style::can_invoke | view_element_style::has_tooltip), _state(state),
	_display(std::move(display))
{
	const auto& data = _display->_selected_item_data;
	_listing = libfiles::list_c64_media({data.data(), data.size()});
	_cols = static_cast<int>(_listing.columns);
	_rows = static_cast<int>(_listing.rows.size());

	const auto* const file_type = _display->_item1 ? _display->_item1->file_type() : nullptr;
	const auto tape = file_type && file_type->extension == "t64";
	const auto disk = file_type && file_type->has_trait(file_traits::disk_image);
	auto runnable = 0;

	for (const auto& row : _listing.rows)
	{
		const auto runs = c64_row_runs(row.file_type, tape);
		_runs.push_back(runs);
		if (runs) ++runnable;
	}

	// A disk is chosen from by its rows. Anything else that holds just one thing to run starts it with a button.
	if (!disk && _rows == 1 && runnable == 1) _start_row = 0;
}

// Scale that fits the cols*8 x rows*8 listing texture into the available width. Prefer an integer DPI-based
// scale for crisp pixels, but shrink so the listing always stays inside the border when the panel is narrow.
double commodore_disk_control::fit_scale(const int inner_width, const double scale_factor) const
{
	const int tex_w = _cols * 8;
	if (tex_w <= 0) return 1.0;

	double scale = std::max(1, core::round(scale_factor * 2.0));
	if (tex_w * scale > inner_width) scale = static_cast<double>(std::max(8, inner_width)) / tex_w;
	return scale;
}

sizei commodore_disk_control::measure(render::measure_context& mc, const int width_limit) const
{
	if (_cols <= 0 || _rows <= 0) return {width_limit, 0};

	const auto scale = fit_scale(width_limit - mc.padding2 * 4, mc.scale_factor);
	auto height = std::max(1, core::round(_rows * 8 * scale)) + mc.padding2 * 4;
	if (_start_row >= 0) height += mc.padding2 * 2 + button_height(mc);
	return {width_limit, height};
}

void commodore_disk_control::layout(render::measure_context& mc, const recti bounds_in)
{
	bounds = bounds_in;
	_scale = fit_scale(bounds.width() - mc.padding2 * 4, mc.scale_factor);

	const auto w = std::max(1, core::round(_cols * 8 * _scale));
	const auto h = std::max(1, core::round(_rows * 8 * _scale));
	const auto left = bounds.left + (bounds.width() - w) / 2;
	const auto top = bounds.top + mc.padding2 * 2;
	_listing_bounds = recti(left, top, left + w, top + h);
	_start_bounds = {};

	if (_start_row >= 0)
	{
		// Below the listing's border, as wide as four lines are tall however short its label.
		const auto bw = std::max(button_width(mc, tt.c64_start), mc.text_line_height(render::font_face::dialog) * 4);
		const auto bx = bounds.left + (bounds.width() - bw) / 2;
		const auto by = _listing_bounds.bottom + mc.padding2 * 4;
		_start_bounds = recti(bx, by, bx + bw, by + button_height(mc));
	}
}

int commodore_disk_control::part_at(const pointi loc) const
{
	if (_start_row >= 0 && _start_bounds.contains(loc)) return start_part;
	if (_rows <= 0 || _scale <= 0.0 || !_listing_bounds.contains(loc)) return no_part;

	// The rows' edges are rounded to pixels, so the one the arithmetic names is checked against its neighbours.
	const auto guess = static_cast<int>((loc.y - _listing_bounds.top) / (8 * _scale));

	for (auto row = std::max(0, guess - 1); row <= std::min(_rows - 1, guess + 1); ++row)
	{
		if (part_bounds(row).contains(loc)) return _runs[row] ? row : no_part;
	}

	return no_part;
}

recti commodore_disk_control::part_bounds(const int part) const
{
	if (part == start_part) return _start_bounds;
	if (part < 0 || part >= _rows) return {};

	const auto top = _listing_bounds.top + core::round(part * 8 * _scale);
	const auto bottom = _listing_bounds.top + core::round((part + 1) * 8 * _scale);
	return {_listing_bounds.left, top, _listing_bounds.right, bottom};
}

std::vector<recti> commodore_disk_control::keyboard_parts() const
{
	std::vector<recti> parts;

	for (auto row = 0; row < _rows && row < static_cast<int>(_runs.size()); ++row)
	{
		if (_runs[row]) parts.emplace_back(part_bounds(row));
	}

	if (_start_row >= 0) parts.emplace_back(_start_bounds);
	return parts;
}

void commodore_disk_control::render(render::draw_context& dc, const pointi element_offset) const
{
	if (_cols <= 0 || _rows <= 0) return;

	if (!_texture || !_texture->is_valid())
	{
		if (const auto surface = files::c64_listing_surface(_listing, c64_light_blue, c64_blue))
		{
			_tex_extent = surface->dimensions();
			_texture = dc.create_texture();
			if (_texture) _texture->update(surface);
		}
	}

	if (!_texture) return;

	const auto alpha = dc.colors.alpha;
	const auto dst = _listing_bounds.offset(element_offset);
	const auto bg_bounds = dst.inflate(dc.padding2);
	const auto border_clr = core::color(c64_light_blue, alpha);

	// Point sampling keeps crisp retro pixels at exact integer scales; use bilinear when the listing is shrunk
	// to a fractional scale so it stays smooth and legible instead of dropping pixel rows.
	const bool integer_scale = std::abs(_scale - std::round(_scale)) < 0.01;
	const auto sampler = integer_scale ? render::texture_sampler::point : render::texture_sampler::bilinear;

	dc.draw_rect(bg_bounds, core::color(c64_blue, alpha));
	dc.draw_border(bg_bounds, bg_bounds.inflate(dc.padding2), border_clr, border_clr);
	dc.draw_texture(_texture, dst, recti(0, 0, _tex_extent.cx, _tex_extent.cy), alpha, sampler);

	if (_hot >= 0 && _hot < _rows)
	{
		if (!_reversed || !_reversed->is_valid())
		{
			if (const auto surface = files::c64_listing_surface(_listing, c64_blue, c64_light_blue))
			{
				_reversed = dc.create_texture();
				if (_reversed) _reversed->update(surface);
			}
		}

		if (_reversed)
		{
			dc.draw_texture(_reversed, part_bounds(_hot).offset(element_offset),
			                recti(0, _hot * 8, _tex_extent.cx, _hot * 8 + 8), alpha, sampler);
		}
	}

	if (_start_row >= 0)
	{
		draw_button(dc, tt.c64_start, _start_bounds.offset(element_offset), _hot == start_part,
		            _pressed && _hot == start_part);
	}
}

void commodore_disk_control::hover(interaction_context& ic)
{
	// The controller tells a pointer that has left with a location of -1, -1.
	const auto gone = ic.loc.x < 0 && ic.loc.y < 0;
	const auto under = gone ? no_part : part_at(ic.loc - ic.element_offset);
	if (track_part(ic, under, no_part, _tracked, _hot, _pressed)) ic.redraw = true;
}

void commodore_disk_control::tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const
{
	const auto part = part_at(loc - element_offset);
	if (part == no_part) return;

	hover.elements->add(std::make_shared<text_element>(tt.c64_run_tooltip));
	hover.active_bounds = hover.window_bounds = part_bounds(part).offset(element_offset);
}

void commodore_disk_control::dispatch_event(const view_element_event& event)
{
	// texture::is_valid() only answers "not null" - it knows nothing about the device that made it - so an
	// element caching one has to answer this broadcast or it draws a texture the device no longer owns.
	if (event.type == view_element_event_type::free_graphics_resources)
	{
		_texture.reset();
		_reversed.reset();
		_tex_extent = {};
	}
	else if (event.type == view_element_event_type::invoke)
	{
		// The controller has just reported where the button went up, and only invokes on its own part.
		const auto row = _hot == start_part ? _start_row : _hot;
		if (row >= 0) _state.start_c64(static_cast<size_t>(row));
	}
}

view_controller_ptr commodore_disk_control::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                                     const pointi element_offset,
                                                                     hit_test_context& /*ctx*/)
{
	const auto part = part_at(loc - element_offset);
	if (part == no_part) return nullptr;

	// One controller for each row, so moving to another row lights that one.
	return std::make_shared<clickable_controller>(host, shared_from_this(), element_offset,
	                                              part_bounds(part).offset(element_offset));
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool is_c64_cartridge(const file_type& type)
{
	return type.has_trait(file_traits::commodore) && core::icmp(type.extension, "crt") == 0;
}

std::string c64_cartridge_title(const libfiles::c64_listing& listing, const std::string_view file_name)
{
	std::string title;

	if (listing.rows.size() == 1)
	{
		// A header's name is ASCII padded with NULs; nothing else in it is a letter worth showing.
		for (const auto c : listing.rows.front().name)
		{
			if (c >= 0x20 && c < 0x7F) title += static_cast<char>(c);
		}
	}

	title = std::string(core::trim(title));
	if (title.empty()) title = std::string(core::trim(file_name));

	// Labels are printed in capitals; bytes past ASCII are left as they are, so a UTF-8 name stays whole.
	std::ranges::transform(title, title.begin(),
	                       [](const char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
	return title;
}

void draw_c64_cartridge(render::draw_context& dc, const recti r, const std::string_view title, const bool hot,
                        render::texture_ptr& shadow)
{
	const auto face_w = std::min(r.width() / (1.0 + depth_x), r.height() / (cartridge_aspect * (1.0 + depth_x)));
	if (face_w < 24.0) return;

	const auto alpha = dc.colors.alpha;
	const auto c = [alpha](const core::color32 rgb, const float a = 1.0f)
	{
		return core::color(rgb, alpha * a);
	};
	const auto line = std::max(1, core::round(dc.scale_factor));
	const auto box = [](const double l, const double t, const double rr, const double b)
	{
		return recti(core::round(l), core::round(t), core::round(rr), core::round(b));
	};
	const auto quad = [&dc](const core::pointd a, const core::pointd b, const core::pointd cc, const core::pointd d,
	                        const core::color colour)
	{
		const core::pointd points[] = {a, b, cc, d};
		// Crisp, so the faces of the shell meet without a seam between them.
		dc.draw_polygon(points, {.fill = colour, .smooth = false});
	};

	const auto dx = face_w * depth_x;
	const auto dy = face_w * depth_y;
	const auto face_h = face_w * shell_length / shell_width;
	const auto drawn_w = face_w + dx;
	const auto drawn_h = drawn_w * cartridge_aspect;
	const auto fl = r.left + (r.width() - drawn_w) / 2.0;
	const auto ft = r.top + (r.height() - drawn_h) / 2.0 + dy;
	const auto fr = fl + face_w;
	const auto fb = ft + face_h;

	// Black plastic lit from the upper left, as the calendar's blocks are: the top end catches the light, the face
	// is the shell's own black, and the right side turns away from it. A little lighter under the pointer.
	const auto lift = hot ? 10 : 0;
	const auto grey = [&](const int v)
	{
		return c(core::rgb(v + lift, v + lift, v + lift + 4));
	};

	// The shadow it stands in, lifting it off the panel: offset down and right, and spreading past its edges.
	// Rasterised once for its size, as the sidebar's charts are, so it is the same picture on every backend.
	const auto spread = cartridge_shadow(r);
	const auto shade = box(fl, ft, fr, fb).offset(spread / 2, spread / 2).inflate(spread * 2);

	if (!shadow || shadow->dimensions() != shade.extent())
	{
		shadow.reset();
		const auto pixels = std::make_shared<core::surface>();

		if (pixels->alloc(shade.extent(), core::texture_format::ARGB))
		{
			pixels->make_blank();
			render::software_canvas(*pixels).fill_shadow(recti(shade.extent()), spread * 2, 0.4f);
			shadow = dc.create_texture();
			if (shadow) shadow->update(pixels);
		}
	}

	if (shadow) dc.draw_texture(shadow, shade, alpha);

	// The board, halfway through the shell, its contacts standing out of the open end below the face: 22 a side,
	// and the slot between the second and the third that keys it into the port.
	const auto board_w = face_w * board_width / shell_width;
	const auto bl = fl + (face_w - board_w) / 2.0 + dx / 2.0;
	const auto br = bl + board_w;
	const auto board_bottom = fb + face_w * board_reach - dy / 2.0;
	const auto pitch = board_w / 22.0;
	const auto board = c(core::rgb(0x2A, 0x52, 0x38));
	dc.draw_rect(box(bl, fb - dy, bl + pitch * 1.85, board_bottom), board);
	dc.draw_rect(box(bl + pitch * 2.15, fb - dy, br, board_bottom), board);

	for (auto i = 0; i < 22; ++i)
	{
		const auto middle = bl + pitch * (i + 0.5);
		dc.draw_rect(box(middle - pitch * 0.3, fb, middle + pitch * 0.3, board_bottom - line),
		             c(core::rgb(0xD2, 0xA8, 0x3E)));
	}

	// The shell: its top end and its right side receding, then the face over them.
	quad({fl, ft}, {fl + dx, ft - dy}, {fr + dx, ft - dy}, {fr, ft}, grey(0x34));
	quad({fr, ft}, {fr + dx, ft - dy}, {fr + dx, fb - dy}, {fr, fb}, grey(0x11));
	dc.draw_rect(box(fl, ft, fr, fb), grey(0x1E));

	// The grip, what is held to pull it out of the port: ridges across the top third of the face, wrapping down
	// the side.
	const auto rib_top = ft + face_h * 0.035;
	const auto rib_pitch = face_h * 0.27 / 9.0;
	const auto rib = std::max(static_cast<double>(line * 2), rib_pitch * 0.5);

	for (auto i = 0; i < 9; ++i)
	{
		const auto y = rib_top + i * rib_pitch;
		dc.draw_rect(box(fl, y, fr, y + line), grey(0x40));
		dc.draw_rect(box(fl, y + line, fr, y + rib), grey(0x27));
		dc.draw_rect(box(fl, y + rib, fr, y + rib + line), grey(0x0C));
		quad({fr, y}, {fr + dx, y - dy}, {fr + dx, y + rib - dy}, {fr, y + rib}, grey(0x19));
		quad({fr, y + rib}, {fr + dx, y + rib - dy}, {fr + dx, y + rib + line - dy}, {fr, y + rib + line}, grey(0x05));
	}

	// The seam where the shell's two halves meet, halfway through it.
	dc.draw_rect(box(fl + dx / 2, ft - dy / 2, fr + dx / 2, ft - dy / 2 + line), grey(0x24));
	dc.draw_rect(box(fr + dx / 2, ft - dy / 2, fr + dx / 2 + line, fb - dy / 2), grey(0x06));

	// The recess molded for the label below the grip, its upper and left walls in shadow and the others lit, and
	// the silver label flush in it.
	const auto inset = face_w * 0.075;
	const auto cavity = box(fl + inset, ft + face_h * 0.36, fr - inset, fb - face_h * 0.07);
	dc.draw_rect(cavity, grey(0x17));
	dc.draw_rect(recti(cavity.left, cavity.top, cavity.right, cavity.top + line * 2), grey(0x08));
	dc.draw_rect(recti(cavity.left, cavity.top, cavity.left + line, cavity.bottom), grey(0x0B));
	dc.draw_rect(recti(cavity.left, cavity.bottom - line, cavity.right, cavity.bottom), grey(0x33));
	dc.draw_rect(recti(cavity.right - line, cavity.top, cavity.right, cavity.bottom), grey(0x2B));

	const auto label = cavity.inflate(-std::max(line * 2, core::round(face_w * 0.02)));
	if (label.width() < 8 || label.height() < 8) return;

	const auto label_radius = std::max(1, label.width() / 40);
	dc.draw_rounded_rect(label, c(core::rgb(0xC2, 0xC4, 0xC9)), label_radius);
	dc.draw_rect(recti(label.left + label_radius, label.top + line, label.right - label_radius,
	                   label.top + label.height() / 3),
	             c(core::rgb(0xD3, 0xD5, 0xD9)));

	// The title in Commodore's blue - on two lines when it needs them - over the maker's name, its rainbow and the
	// model.
	const auto pad = std::max(line * 3, label.width() / 14);
	const auto text_width = std::max(1, label.width() - pad * 2);
	const auto title_line = dc.text_line_height(render::font_face::title);
	const auto maker_height = dc.text_line_height(render::font_face::dialog);
	auto title_style = render::text_style::multiline_center;
	auto title_height = dc.measure_text(title, render::font_face::title, title_style, text_width).cy;

	if (title_height > title_line * 2)
	{
		title_style = render::text_style::single_line_center;
		title_height = title_line;
	}

	const auto dark = c(core::rgb(0x34, 0x34, 0x38));
	constexpr std::string_view maker = "commodore";
	constexpr std::string_view model = "64";
	const auto maker_width =
		dc.measure_text(maker, render::font_face::dialog, render::text_style::single_line, text_width).cx;
	const auto model_width =
		dc.measure_text(model, render::font_face::dialog, render::text_style::single_line, text_width).cx;
	const auto stripe_height = std::max(4, maker_height * 3 / 4);
	const auto stripe_width = std::max(line * 2, stripe_height / 5);
	const auto slant = stripe_height / 2;
	constexpr core::color32 rainbow[] = {core::rgb(0xE0, 0x45, 0x3A), core::rgb(0xF0, 0x8A, 0x2C),
	                                     core::rgb(0xF2, 0xC2, 0x30), core::rgb(0x4A, 0xAD, 0x4F),
	                                     core::rgb(0x2C, 0x8E, 0xD6)};
	const auto rainbow_width = stripe_width * static_cast<int>(std::size(rainbow)) + slant;
	const auto space = std::max(line * 2, pad / 2);

	// The maker's name, its rainbow and the model share a line where they fit. In a narrower label the name takes
	// a line of its own above the other two, and where even that is too narrow it gives way, then the rainbow, so
	// the model always shows.
	const auto one_line = maker_width + space + rainbow_width + space + model_width <= text_width;
	const auto own_line = !one_line && maker_width <= text_width;
	const auto with_rainbow = rainbow_width + space + model_width <= text_width;
	const auto gap = maker_height / 3;
	const auto block = title_height + gap + maker_height * (own_line ? 2 : 1);
	const auto text_top = label.top + std::max(pad / 2, (label.height() - block) / 2);
	const recti title_bounds(label.left + pad, text_top, label.right - pad, text_top + title_height);
	dc.draw_text(title, title_bounds, render::font_face::title, title_style, c(core::rgb(0x16, 0x7C, 0xC4)), {});

	auto maker_top = title_bounds.bottom + gap;

	if (own_line)
	{
		const auto left = label.left + (label.width() - maker_width) / 2;
		dc.draw_text(maker, recti(left, maker_top, left + maker_width, maker_top + maker_height),
		             render::font_face::dialog, render::text_style::single_line, dark, {});
		maker_top += maker_height;
	}

	const auto whole = (one_line ? maker_width + space : 0) + (with_rainbow ? rainbow_width + space : 0) + model_width;
	auto x = label.left + (label.width() - whole) / 2;

	if (one_line)
	{
		dc.draw_text(maker, recti(x, maker_top, x + maker_width, maker_top + maker_height), render::font_face::dialog,
		             render::text_style::single_line, dark, {});
		x += maker_width + space;
	}

	if (with_rainbow)
	{
		const auto top = static_cast<double>(maker_top + (maker_height - stripe_height) / 2);
		const auto bottom = top + stripe_height;

		for (const auto colour : rainbow)
		{
			quad({static_cast<double>(x + slant), top}, {static_cast<double>(x + slant + stripe_width), top},
			     {static_cast<double>(x + stripe_width), bottom}, {static_cast<double>(x), bottom}, c(colour));
			x += stripe_width;
		}

		x += slant + space;
	}

	dc.draw_text(model, recti(x, maker_top, x + model_width, maker_top + maker_height), render::font_face::dialog,
	             render::text_style::single_line, dark, {});
}

c64_cartridge_control::c64_cartridge_control(view_state& state, display_state_ptr display) :
	view_element(view_element_style::can_invoke | view_element_style::has_tooltip | flex_item::stretch), _state(state),
	_display(std::move(display))
{
	const auto& data = _display->_selected_item_data;
	const auto listing = libfiles::list_c64_media({data.data(), data.size()});
	const auto name = _display->_item1 ? std::string(_display->_item1->path().file_name_without_extension())
	                                   : std::string{};
	_title = c64_cartridge_title(listing, name);
	if (listing.rows.size() == 1 && c64_row_runs(listing.rows.front().file_type, false)) _row = 0;
}

sizei c64_cartridge_control::measure(render::measure_context& mc, const int width_limit) const
{
	const auto pad = core::round(ui::panel_element::content_padding * mc.scale_factor);
	const auto width = cartridge_width(mc, std::max(0, width_limit - pad * 2));
	const auto height = cartridge_height(mc, width);
	const auto gap = std::max(mc.padding2 * 2, cartridge_shadow(recti(0, 0, width, height)) * 2);
	return {width_limit, pad * 2 + height + (runs() ? gap + button_height(mc) : 0)};
}

void c64_cartridge_control::layout(render::measure_context& mc, const recti bounds_in)
{
	bounds = bounds_in;

	const auto pad = core::round(ui::panel_element::content_padding * mc.scale_factor);
	const auto width = cartridge_width(mc, std::max(0, bounds.width() - pad * 2));
	const auto height = cartridge_height(mc, width);
	const auto gap = std::max(mc.padding2 * 2, cartridge_shadow(recti(0, 0, width, height)) * 2);
	const auto block = height + (runs() ? gap + button_height(mc) : 0);
	const auto left = bounds.left + (bounds.width() - width) / 2;
	const auto top = bounds.top + std::max(pad, (bounds.height() - block) / 2);
	_cartridge = recti(left, top, left + width, top + height);
	_start = {};

	if (runs())
	{
		const auto button_width = std::max(start_button_width(mc), mc.text_line_height(render::font_face::dialog) * 5);
		const auto button_left = bounds.left + (bounds.width() - button_width) / 2;
		_start = recti(button_left, _cartridge.bottom + gap, button_left + button_width,
		               _cartridge.bottom + gap + button_height(mc));
	}
}

c64_cartridge_control::part c64_cartridge_control::part_at(const pointi loc) const
{
	if (!runs()) return part::none;
	if (_start.contains(loc)) return part::start;
	if (_cartridge.contains(loc)) return part::cartridge;
	return part::none;
}

recti c64_cartridge_control::part_bounds(const part p) const
{
	if (p == part::start) return _start;
	if (p == part::cartridge) return _cartridge;
	return {};
}

std::vector<recti> c64_cartridge_control::keyboard_parts() const
{
	if (!runs() || _start.is_empty()) return {};
	return {_start};
}

void c64_cartridge_control::render(render::draw_context& dc, const pointi element_offset) const
{
	draw_stack_panel(dc, bounds.offset(element_offset));
	draw_c64_cartridge(dc, _cartridge.offset(element_offset), _title, _hot != part::none, _shadow);

	if (runs())
	{
		draw_start_button(dc, _start.offset(element_offset), _hot != part::none, _pressed && _hot != part::none);
	}
}

void c64_cartridge_control::hover(interaction_context& ic)
{
	const auto gone = ic.loc.x < 0 && ic.loc.y < 0;
	const auto under = gone ? part::none : part_at(ic.loc - ic.element_offset);
	if (track_part(ic, under, part::none, _tracked, _hot, _pressed)) ic.redraw = true;
}

void c64_cartridge_control::tooltip(view_hover_element& hover, const pointi loc, const pointi element_offset) const
{
	const auto p = part_at(loc - element_offset);
	if (p == part::none) return;

	hover.elements->add(std::make_shared<text_element>(tt.c64_run_tooltip));
	hover.active_bounds = hover.window_bounds = part_bounds(p).offset(element_offset);
}

void c64_cartridge_control::dispatch_event(const view_element_event& event)
{
	// A texture knows nothing of the device that made it, so the cached shadow goes with the device.
	if (event.type == view_element_event_type::free_graphics_resources)
	{
		_shadow.reset();
	}
	// The controller has just reported where the button went up, and only invokes on its own part.
	else if (event.type == view_element_event_type::invoke && _hot != part::none && runs())
	{
		_state.start_c64(static_cast<size_t>(_row));
	}
}

view_controller_ptr c64_cartridge_control::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                                    const pointi element_offset,
                                                                    hit_test_context& /*ctx*/)
{
	const auto p = part_at(loc - element_offset);
	if (p == part::none) return nullptr;

	return std::make_shared<clickable_controller>(host, shared_from_this(), element_offset,
	                                              part_bounds(p).offset(element_offset));
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

c64_screen_control::c64_screen_control(view_state& state, display_state_ptr display) :
	view_element(view_element_style::can_invoke | flex_item::media), _state(state), _display(std::move(display))
{
}

sizei c64_screen_control::measure(render::measure_context& mc, const int width_limit) const
{
	bool whole = true;
	const auto picture = fit_c64_picture({width_limit, width_limit * 3}, whole);
	return {width_limit, picture.cy + mc.padding2 + button_height(mc)};
}

void c64_screen_control::layout(render::measure_context& mc, const recti bounds_in)
{
	bounds = bounds_in;

	const auto gap = mc.padding2;
	const auto row_height = button_height(mc);
	const auto picture = fit_c64_picture({bounds.width(), std::max(0, bounds.height() - gap - row_height)},
	                                     _whole_scale);
	const auto block = picture.cy + gap + row_height;
	const auto left = bounds.left + (bounds.width() - picture.cx) / 2;
	const auto top = bounds.top + std::max(0, (bounds.height() - block) / 2);
	_picture = recti(left, top, left + picture.cx, top + picture.cy);

	// Back and the port under the picture's left edge - or the element's, when the picture is narrower than
	// they are - and the keys it takes at the other end, when they fit.
	const auto back_width = button_width(mc, tt.command_browse_back);
	const auto port_width = button_width(mc, port_label(2));
	const auto under_picture = picture.cx >= back_width + gap + port_width;
	const auto row_left = under_picture ? _picture.left : bounds.left;
	const auto row_right = under_picture ? _picture.right : bounds.right;
	const auto row_top = _picture.bottom + gap;

	_back = recti(row_left, row_top, row_left + back_width, row_top + row_height);
	_port = recti(_back.right + gap, row_top, _back.right + gap + port_width, row_top + row_height);

	const auto hint_width =
		mc.measure_text(tt.c64_keys_help, render::font_face::dialog, render::text_style::single_line, 10000).cx;
	_hint = row_right - _port.right - gap * 2 >= hint_width
	            ? recti(row_right - hint_width, row_top, row_right, row_top + row_height)
	            : recti{};
}

c64_screen_control::part c64_screen_control::part_at(const pointi loc) const
{
	if (_back.contains(loc)) return part::back;

	const auto session = _display->_c64;
	if (session && session->state() != c64_session::status::failed && _port.contains(loc)) return part::port;
	return part::none;
}

recti c64_screen_control::part_bounds(const part p) const
{
	if (p == part::back) return _back;
	if (p == part::port) return _port;
	return {};
}

std::vector<recti> c64_screen_control::keyboard_parts() const
{
	std::vector<recti> parts;
	if (!_back.is_empty()) parts.emplace_back(_back);

	const auto session = _display->_c64;
	if (session && session->state() != c64_session::status::failed && !_port.is_empty()) parts.emplace_back(_port);
	return parts;
}

void c64_screen_control::render(render::draw_context& dc, const pointi element_offset) const
{
	const auto& d = _display;
	const auto session = d->_c64;
	// Back and Escape end the session before the elements are rebuilt, so a frame can be drawn in between with no
	// machine at all: that is black, as a machine switched off is, rather than one that could not start.
	const auto failed = session && session->state() == c64_session::status::failed;
	const auto running = session && !failed;
	const auto alpha = dc.colors.alpha;
	const auto picture = _picture.offset(element_offset);
	d->_c64_picture_bounds = picture;

	if (running)
	{
		// The display brings the texture up to the newest picture for each present; it is only made here.
		if (!d->_c64_texture)
		{
			d->_c64_texture = dc.create_texture();
			d->_c64_frame_number = 0;
		}

		uint64_t number = 0;
		const auto frame = session->frame(number);

		if (frame && d->_c64_texture && number != d->_c64_frame_number)
		{
			d->_c64_texture->update(frame);
			d->_c64_frame_number = number;
		}
	}

	if (running && d->_c64_texture && d->_c64_frame_number > 0)
	{
		dc.draw_texture(d->_c64_texture, picture, recti(0, 0, c64_session::picture_width, c64_session::picture_height),
		                alpha, _whole_scale ? render::texture_sampler::point : render::texture_sampler::bilinear);
	}
	else
	{
		// Black until the machine has drawn its first picture, as a monitor is while a C64 powers up.
		dc.draw_rect(picture, core::color(core::rgb(0, 0, 0), alpha));
	}

	const auto text = core::color(ui::style::colors.view_text, alpha);

	if (failed)
	{
		dc.draw_text(tt.c64_failed, picture.inflate(-dc.padding2), render::font_face::dialog,
		             render::text_style::multiline_center, text, {});
	}

	draw_button(dc, tt.command_browse_back, _back.offset(element_offset), _hot == part::back,
	            _pressed && _hot == part::back);

	if (running)
	{
		draw_button(dc, port_label(session->joystick_port()), _port.offset(element_offset), _hot == part::port,
		            _pressed && _hot == part::port);

		if (!_hint.is_empty())
		{
			const auto muted = core::color(core::lerp(ui::style::colors.view_text, ui::style::colors.view_background,
			                                          0.35f),
			                               alpha);
			dc.draw_text(tt.c64_keys_help, _hint.offset(element_offset), render::font_face::dialog,
			             render::text_style::single_line_far, muted, {});
		}
	}
}

void c64_screen_control::hover(interaction_context& ic)
{
	const auto gone = ic.loc.x < 0 && ic.loc.y < 0;
	const auto under = gone ? part::none : part_at(ic.loc - ic.element_offset);
	if (track_part(ic, under, part::none, _tracked, _hot, _pressed)) ic.redraw = true;
}

void c64_screen_control::dispatch_event(const view_element_event& event)
{
	if (event.type == view_element_event_type::free_graphics_resources)
	{
		_display->_c64_texture.reset();
	}
	else if (event.type == view_element_event_type::invoke)
	{
		if (_hot == part::back)
		{
			_state.stop_c64();
		}
		else if (_hot == part::port && _display->_c64)
		{
			_state.c64_joystick_port(_display->_c64->joystick_port() == 1 ? 2 : 1);
		}
	}
}

view_controller_ptr c64_screen_control::controller_from_location(const view_host_ptr& host, const pointi loc,
                                                                 const pointi element_offset, hit_test_context& /*ctx*/)
{
	const auto p = part_at(loc - element_offset);
	if (p == part::none) return nullptr;

	return std::make_shared<clickable_controller>(host, shared_from_this(), element_offset,
	                                              part_bounds(p).offset(element_offset));
}
