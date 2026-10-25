// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The application's embedded assets - the index database's schema and the world map - linked
// in from arrays the build generates from src/Res, the same bytes on every platform, and its mark,
// drawn at whatever size it is wanted.

#include "pch.h"
#include "app_resources.h"

// Generated from src/Res by CMake; see the resource block in CMakeLists.txt.
extern const unsigned char diffractor_resource_sql[];
extern const unsigned long diffractor_resource_sql_size;
extern const unsigned char diffractor_resource_map_png[];
extern const unsigned long diffractor_resource_map_png_size;

df::blob load_app_resource(const app_resource r)
{
	switch (r)
	{
	case app_resource::sql:
		return {diffractor_resource_sql, diffractor_resource_sql + diffractor_resource_sql_size};
	case app_resource::map_png:
		return {diffractor_resource_map_png, diffractor_resource_map_png + diffractor_resource_map_png_size};
	}

	return {};
}

namespace
{
	// The Diffractor mark: four interlocking squares in a pinwheel. This is the drawing of record
	// for anything rendered at runtime. tools/generate_store_assets.py draws the same mark for the
	// packaged artwork (app.ico and the Store assets); the two must be changed together.
	struct logo_square
	{
		double cx;
		double cy;
		ui::color32 fill;
		ui::color32 border;
	};

	// Supersample then box-average. Every edge is axis aligned, so this resolves them exactly and
	// avoids the dark fringe a straight RGBA average produces around transparent pixels.
	constexpr int logo_supersample = 4;

	// Surface pixels are stored blue first (ARGB maps to DXGI_FORMAT_B8G8R8A8), while ui::rgb builds
	// the red-first form these brand hex values are written in.
	constexpr ui::color32 logo_color(const uint32_t r, const uint32_t g, const uint32_t b) noexcept
	{
		return ui::abgr(ui::rgb(r, g, b));
	}

	void logo_fill_rect(ui::color32* buffer, const int n, int x0, int y0, int x1, int y1, const ui::color32 c,
	                    const recti clip)
	{
		x0 = std::max({x0, 0, clip.left});
		y0 = std::max({y0, 0, clip.top});
		x1 = std::min({x1, n - 1, clip.right - 1});
		y1 = std::min({y1, n - 1, clip.bottom - 1});

		for (auto y = y0; y <= y1; ++y)
		{
			auto* const line = buffer + static_cast<ptrdiff_t>(y) * n;

			for (auto x = x0; x <= x1; ++x)
			{
				line[x] = c;
			}
		}
	}

	void logo_draw_square(ui::color32* buffer, const int n, const logo_square& sq, const double size,
	                      const int border_width, const recti clip)
	{
		const auto half = size / 2.0;
		const auto x0 = df::round(sq.cx - half);
		const auto y0 = df::round(sq.cy - half);
		const auto x1 = df::round(sq.cx + half);
		const auto y1 = df::round(sq.cy + half);

		logo_fill_rect(buffer, n, x0, y0, x1, y1, sq.border, clip);
		logo_fill_rect(buffer, n, x0 + border_width, y0 + border_width, x1 - border_width, y1 - border_width, sq.fill,
		               clip);
	}
}

void fill_app_logo(ui::surface& surface)
{
	const auto cx = static_cast<int>(surface.width());
	const auto cy = static_cast<int>(surface.height());
	const auto extent = std::min(cx, cy);
	if (extent <= 0) return;

	const auto n = extent * logo_supersample;
	std::vector<ui::color32> buffer(static_cast<size_t>(n) * n, 0u);

	const auto size = n * 0.45;
	const auto offset = size * 1.11;
	const auto middle = n / 2.0;
	const auto border_width = std::max(1, static_cast<int>(size * 0.08));
	const recti no_clip{0, 0, n, n};

	const logo_square green{middle, middle - offset / 2.0, logo_color(0x00, 0xb0, 0x50), logo_color(0x00, 0x8a, 0x3e)};
	const logo_square blue{middle + offset / 2.0, middle, logo_color(0x00, 0x70, 0xc0), logo_color(0x00, 0x54, 0x90)};
	const logo_square red{middle, middle + offset / 2.0, logo_color(0xc0, 0x00, 0x00), logo_color(0x90, 0x00, 0x00)};
	const logo_square yellow{middle - offset / 2.0, middle, logo_color(0xff, 0xc0, 0x00), logo_color(0xbf, 0x90, 0x00)};

	// Painter's order gives yellow over green; the weave needs green back on top where it meets
	// yellow, so its lower-left quadrant is redrawn last.
	logo_draw_square(buffer.data(), n, green, size, border_width, no_clip);
	logo_draw_square(buffer.data(), n, blue, size, border_width, no_clip);
	logo_draw_square(buffer.data(), n, red, size, border_width, no_clip);
	logo_draw_square(buffer.data(), n, yellow, size, border_width, no_clip);

	const auto half = size / 2.0;
	const recti green_patch{
		df::round(green.cx - half - border_width), df::round(green.cy),
		df::round(green.cx), df::round(green.cy + half + border_width)
	};
	logo_draw_square(buffer.data(), n, green, size, border_width, green_patch);

	const auto left = (cx - extent) / 2;
	const auto top = (cy - extent) / 2;
	constexpr auto samples = logo_supersample * logo_supersample;

	for (auto y = 0; y < cy; ++y)
	{
		auto* const line = std::bit_cast<ui::color32*>(surface.pixels_line(y));

		for (auto x = 0; x < cx; ++x)
		{
			const auto sx = (x - left) * logo_supersample;
			const auto sy = (y - top) * logo_supersample;

			if (sx < 0 || sy < 0 || sx >= n || sy >= n)
			{
				line[x] = 0;
				continue;
			}

			uint32_t r = 0, g = 0, b = 0, opaque = 0;

			for (auto j = 0; j < logo_supersample; ++j)
			{
				const auto* const src = buffer.data() + static_cast<ptrdiff_t>(sy + j) * n + sx;

				for (auto i = 0; i < logo_supersample; ++i)
				{
					const auto c = src[i];
					if (c == 0) continue;
					r += ui::get_r(c);
					g += ui::get_g(c);
					b += ui::get_b(c);
					++opaque;
				}
			}

			// Channels are averaged in storage order, so this stays blue first like its source.
			line[x] = opaque == 0
				          ? 0
				          : ui::rgba(r / opaque, g / opaque, b / opaque, opaque * 255 / samples);
		}
	}
}
