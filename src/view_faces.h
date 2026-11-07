// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The face view - every face group the collection holds, as a scrolling list of rows
// carrying the group's sample face, the file that face came from, the number of files the group
// appears in, when it was photographed and where. Owns its list rather than deriving from
// list_view, which has no image column and is shared with Rename, Import and Sync. The settings and
// the explanation sit in the controls panel beside it, which is where the face options dialog went.
// Owned by docs/faces.md.

#pragma once

#include "view_list.h"
#include "app_util.h"
#include "model_faces.h"

// Crops already taken, keyed by the face they were taken from. A crop depends only on that face, so
// one already held stays correct across a refresh and need not be read and cut again.
struct cached_face_thumbnail
{
	faces::palette_entry entry;
	ui::const_surface_ptr pixels;
};

using face_thumbnail_cache = df::hash_map<faces::face_ref, cached_face_thumbnail, faces::face_ref_hash>;

class faces_view final : public view_base, public std::enable_shared_from_this<faces_view>
{
	struct row
	{
		faces::palette_entry entry;
		// Decoded on a worker and published here; the texture is made on the next paint because a
		// device resource may only be created on the UI thread.
		ui::const_surface_ptr thumbnail;
		mutable ui::texture_ptr tex;
		recti bounds;
	};

	view_state& _state;
	view_host_ptr _host;
	view_scroller _scroller;
	std::vector<row> _rows;
	std::string _title;
	std::string _status;
	sizei _extent;
	int _header_height = 0;
	// One refresh in flight and at most one waiting: detection republishes every few files, and each
	// publish would otherwise start a walk of every group plus a thumbnail read per row.
	uint64_t _request = 0;
	bool _in_flight = false;
	bool _again = false;
	bool _active = false;
	std::pair<size_t, size_t> _thumbnail_window{};
	bool _enabled = setting.faces.enabled;
	// False while the collection is still being read, which is the difference between "nobody" and
	// "not yet" - and saying the first while the second is true is a lie the user can see.
	bool _complete = false;
	// Files already asked for a thumbnail, so a refresh does not re-request the whole list every time
	// the index publishes.
	df::hash_set<df::file_path, df::ihash, df::ieq> _thumbnail_requests;

	static constexpr int thumbnail_extent = 48;
	static constexpr int row_padding = 4;

	static int row_height()
	{
		return thumbnail_extent + row_padding * 2;
	}

	// Sample face, then the file it came from, count, dates and places. The file takes what the
	// fixed columns leave, so a long name is what shrinks when the window does.
	std::array<recti, 5> columns(const recti r) const
	{
		const auto text_left = r.left + thumbnail_extent + row_padding * 2;
		const auto avail = std::max(0, r.right - text_left - row_padding);
		const auto count_cx = std::min(avail / 6, 80);
		const auto dates_cx = std::min(avail / 3, 220);
		const auto places_cx = std::min(avail / 4, 200);
		const auto name_cx = std::max(0, avail - count_cx - dates_cx - places_cx);

		auto x = text_left;
		const recti name{x, r.top, x + name_cx, r.bottom};
		x += name_cx;
		const recti count{x, r.top, x + count_cx, r.bottom};
		x += count_cx;
		const recti dates{x, r.top, x + dates_cx, r.bottom};
		x += dates_cx;
		const recti places{x, r.top, x + places_cx, r.bottom};

		return {
			recti{r.left + row_padding, r.top + row_padding, r.left + row_padding + thumbnail_extent, r.bottom - row_padding},
			name, count, dates, places
		};
	}

	// Text reads from the left edge of its column and a count reads from the right, so a name sits
	// beside its thumbnail rather than floating in the middle of whatever width is spare.
	static ui::style::text_style column_style(const size_t column)
	{
		return column == 1 ? ui::style::text_style::single_line_far : ui::style::text_style::single_line;
	}

	static std::string date_range(const faces::palette_entry& e)
	{
		if (!e.first_seen.is_valid()) return {};

		const auto first = platform::format_date(e.first_seen);
		const auto last = platform::format_date(e.last_seen);
		return first == last ? first : std::format("{} - {}", first, last);
	}

	static std::string places(const faces::palette_entry& e)
	{
		std::string result;

		for (const auto& p : e.places)
		{
			if (!result.empty()) result += ", ";
			result += p;
		}

		return result;
	}

	void publish(uint64_t request, uint64_t generation, std::vector<faces::palette_entry> entries,
	             std::vector<ui::const_surface_ptr> surfaces);
	void update_status();
	void finish_refresh();

	std::pair<size_t, size_t> thumbnail_window() const
	{
		const auto visible = static_cast<size_t>(std::max(1, (_extent.cy - _header_height) / row_height() + 2));
		const auto first = static_cast<size_t>(std::max(0, _scroller.scroll_offset().y / row_height()));
		return {first > visible ? first - visible : 0, first + visible * 2};
	}

	// The header does not scroll: a list this long is unreadable if the only thing saying which column
	// is which has been scrolled off the top.
	int header_height(ui::measure_context& mc) const
	{
		return mc.text_line_height(ui::style::font_face::dialog) + row_padding * 2;
	}

public:
	faces_view(view_state& state, view_host_ptr host) : _state(state), _host(std::move(host))
	{
		_scroller.changed_func = [this] { if (_active) refresh(); };
	}

	void activate(sizei extent) override;

	view_bars bars() const override
	{
		return task_view_bars({});
	}

	void deactivate() override
	{
		_active = false;
		_rows.clear();
		_status.clear();
		++_request;
	}

	void refresh() override;

	void layout(ui::measure_context& mc, const sizei extent) override
	{
		_extent = extent;
		_header_height = header_height(mc);

		const recti scroll_bounds{extent.cx - mc.scroll_width, _header_height, extent.cx, extent.cy};
		const recti client_bounds{0, _header_height, std::max(0, extent.cx - mc.scroll_width), extent.cy};

		auto y = client_bounds.top;

		for (auto& r : _rows)
		{
			r.bounds = recti{client_bounds.left, y, client_bounds.right, y + row_height()};
			y += row_height();
		}

		_scroller.layout({client_bounds.width(), y - client_bounds.top}, client_bounds, scroll_bounds);
		if (_active && thumbnail_window() != _thumbnail_window)
			_state.invalidate_view(view_invalid::faces_view);
	}

	void render(ui::draw_context& dc, view_controller_ptr controller) override
	{
		const auto offset = _scroller.scroll_offset();
		const recti header{0, 0, _extent.cx, _header_height};
		const recti clip{0, _header_height, _extent.cx, _extent.cy};

		const auto title_color = ui::color(dc.colors.foreground, dc.colors.alpha * 0.8f);
		const auto header_cols = columns(header);
		const std::array<std::string_view, 4> titles{
			tt.prop_name_filename.sv(), tt.count.sv(), tt.dates_title.sv(), tt.prop_name_place.sv()
		};

		for (auto i = 0u; i < titles.size(); ++i)
		{
			dc.draw_text(titles[i], header_cols[i + 1].inflate(-row_padding * 2, 0),
			             ui::style::font_face::dialog, column_style(i), title_color, {});
		}

		if (_rows.empty())
		{
			dc.draw_text(_complete ? tt.faces_none.sv() : tt.faces_partial.sv(), clip,
			             ui::style::font_face::dialog, ui::style::text_style::multiline_center,
			             ui::color(dc.colors.foreground, dc.colors.alpha * 0.7f), {});
			return;
		}

		for (const auto& r : _rows)
		{
			const auto bounds = r.bounds.offset(0, -offset.y);
			if (bounds.bottom < clip.top || bounds.top > clip.bottom) continue;

			const auto cols = columns(bounds);

			if (ui::is_valid(r.thumbnail))
			{
				if (!r.tex)
				{
					r.tex = dc.create_texture();
					if (r.tex) r.tex->update(r.thumbnail);
				}

				if (r.tex) dc.draw_texture(r.tex, cols[0], dc.colors.alpha);
			}
			else
			{
				dc.draw_rect(cols[0], ui::color(dc.colors.background, dc.colors.alpha * 0.5f));
			}

			const auto text_color = ui::color(dc.colors.foreground, dc.colors.alpha);
			const auto dim_color = ui::color(dc.colors.foreground, dc.colors.alpha * 0.7f);

			// A group has no name. What identifies it is the face on the left and the picture that
			// face came from, which is also what a search for it spells.
			dc.draw_text(r.entry.term, cols[1].inflate(-row_padding * 2, 0),
			             ui::style::font_face::dialog, column_style(0), text_color, {});
			dc.draw_text(std::to_string(r.entry.file_count), cols[2].inflate(-row_padding * 2, 0),
			             ui::style::font_face::dialog, column_style(1), dim_color, {});
			dc.draw_text(date_range(r.entry), cols[3].inflate(-row_padding * 2, 0),
			             ui::style::font_face::dialog, column_style(2), dim_color, {});
			dc.draw_text(places(r.entry), cols[4].inflate(-row_padding * 2, 0),
			             ui::style::font_face::dialog, column_style(3), dim_color, {});
		}

		_scroller.draw_scroll(dc);
	}

	bool mouse_wheel(const pointi loc, const ui::wheel_notch notch) override
	{
		_scroller.offset(_host, 0, -notch.delta);
		return _scroller.can_scroll();
	}

	bool can_scroll(const pointi loc) const override
	{
		return _scroller.can_scroll();
	}

	view_controller_ptr controller_from_location(const view_host_ptr& host, pointi loc,
	                                             hit_test_context& ctx) override;

	void exit() override
	{
		_state.view_mode(view_type::items);
	}

	std::string_view title() override
	{
		_title = std::format("{}: {}", s_app_name, tt.faces_title.sv());
		return _title;
	}

	std::string_view status() override
	{
		update_status();
		return _status;
	}

	// Clicking a row searches for that group, which is the only thing a row can do: a group is its
	// sample face, so the term is a picture and a position in it that the user can see, save and
	// type back.
	void search_for(const view_host_ptr& host, const faces::palette_entry& entry) const
	{
		if (entry.term.empty()) return;
		_state.open(host, df::search_t::parse(df::format_face_term(entry.term)), {});
	}

	view_controls_host_ptr controls(const ui::control_frame_ptr& owner);
};
