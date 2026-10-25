// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Software rasterisers for the sidebar's file-type pie and history calendar. Both draw an
// extruded solid lit by the globe's light into a surface the caller uploads as a texture, and both
// record what each pixel shows so the pointer can be matched against the silhouette rather than
// against the geometry it was drawn from. Backend-independent by construction: the pixels are the
// same under D3D11 and CPU rendering.

#include "pch.h"
#include "ui_charts.h"

namespace
{
	// Colours are written opaque inside the solid and transparent black outside it, so the box
	// reduction produces premultiplied edges for free - the same convention the globe's limb uses.
	inline ui::color32 shade(const ui::color32 c, const double factor)
	{
		const auto q = static_cast<uint32_t>(std::clamp(factor, 0.0, 1.0) * 256.0);
		const auto rb = (((c & 0x00FF00FFu) * q) >> 8) & 0x00FF00FFu;
		const auto g = (((c & 0x0000FF00u) * q) >> 8) & 0x0000FF00u;
		return 0xFF000000u | rb | g;
	}

	// Maximum error is about 0.005 radians, a fiftieth of the 5.6 degrees one pie segment spans.
	// std::atan2 costs more than the rest of the pixel put together at this resolution, and the
	// pie is re-rasterised every time the pointer crosses a wedge.
	inline double fast_atan2(const double y, const double x)
	{
		if (x == 0.0 && y == 0.0) return 0.0;

		const auto ax = std::abs(x);
		const auto ay = std::abs(y);
		const auto a = std::min(ax, ay) / std::max(ax, ay);
		const auto s = a * a;

		auto r = ((-0.0464964749 * s + 0.15931422) * s - 0.327622764) * s * a + a;

		if (ay > ax) r = 1.57079632679 - r;
		if (x < 0.0) r = M_PI - r;
		return y < 0.0 ? -r : r;
	}

	// Fills a convex quad, writing the identity for whichever subsample lands on a 1x pixel. Faces
	// are small parallelograms, so a scanline span costs less than the per-pixel tests the pie
	// needs, and the supersample carries the edges.
	void fill_convex(ui::surface& surface, uint16_t* ids, const sizei extent, const std::array<pointi, 4>& pts,
	                 const ui::color32 color, const uint16_t id)
	{
		const auto dims = surface.dimensions();

		auto y_first = pts[0].y;
		auto y_last = pts[0].y;

		for (const auto& p : pts)
		{
			y_first = std::min(y_first, p.y);
			y_last = std::max(y_last, p.y);
		}

		y_first = std::max(y_first, 0);
		y_last = std::min(y_last, dims.cy - 1);

		for (auto y = y_first; y <= y_last; ++y)
		{
			const auto yc = y + 0.5;
			auto x_min = std::numeric_limits<double>::max();
			auto x_max = std::numeric_limits<double>::lowest();

			for (auto i = 0; i < 4; ++i)
			{
				const auto a = pts[i];
				const auto b = pts[(i + 1) & 3];
				if (a.y == b.y) continue;
				if (yc < std::min(a.y, b.y) || yc >= std::max(a.y, b.y)) continue;

				const auto t = (yc - a.y) / static_cast<double>(b.y - a.y);
				const auto x = a.x + t * (b.x - a.x);
				x_min = std::min(x_min, x);
				x_max = std::max(x_max, x);
			}

			if (x_min > x_max) continue;

			const auto x_start = std::max(0, static_cast<int>(std::ceil(x_min - 0.5)));
			const auto x_end = std::min(dims.cx - 1, static_cast<int>(std::floor(x_max - 0.5)));
			if (x_start > x_end) continue;

			auto* const line = std::bit_cast<ui::color32*>(surface.pixels_line(y));
			for (auto x = x_start; x <= x_end; ++x) line[x] = color;

			if (id != chart_surface::no_id && (y & 1) == 0)
			{
				auto* const id_line = ids + static_cast<size_t>(y / chart_supersample) * extent.cx;
				for (auto x = x_start + (x_start & 1); x <= x_end; x += chart_supersample)
				{
					id_line[x / chart_supersample] = id;
				}
			}
		}
	}
}

bool chart_surface::prepare(const sizei extent)
{
	if (extent.cx < 4 || extent.cy < 4) return false;

	const sizei supersampled(extent.cx * chart_supersample, extent.cy * chart_supersample);

	// is_valid, not the pointer or the size: a failed alloc still reports the requested extent, so a
	// dimension compare alone recognises the empty buffer as the right one and never asks again -
	// one transient failure would then stop this chart drawing for the life of the process.
	if (!ui::is_valid(_scratch) || _scratch->dimensions() != supersampled)
	{
		_scratch = std::make_shared<ui::surface>();
		_scratch->alloc(supersampled, ui::texture_format::ARGB);
	}

	if (!ui::is_valid(_pixels) || _pixels->dimensions() != extent)
	{
		_pixels = std::make_shared<ui::surface>();
		_pixels->alloc(extent, ui::texture_format::ARGB);
	}

	if (_extent != extent)
	{
		_ids.assign(static_cast<size_t>(extent.cx) * extent.cy, no_id);
		_extent = extent;
	}

	return is_ready();
}

void chart_surface::clear()
{
	if (!is_ready()) return;
	memset(_scratch->pixels(), 0, _scratch->size());
	std::ranges::fill(_ids, no_id);
}

void chart_surface::reduce() const
{
	if (!is_ready()) return;

	static_assert(chart_supersample == 2, "the box reduction below is written for a 2x supersample");

	for (auto y = 0; y < _extent.cy; ++y)
	{
		const auto* const src0 = std::bit_cast<const ui::color32*>(_scratch->pixels_line(y * 2));
		const auto* const src1 = std::bit_cast<const ui::color32*>(_scratch->pixels_line(y * 2 + 1));
		auto* const dst = std::bit_cast<ui::color32*>(_pixels->pixels_line(y));

		for (auto x = 0; x < _extent.cx; ++x)
		{
			const auto a = src0[x * 2];
			const auto b = src0[x * 2 + 1];
			const auto c = src1[x * 2];
			const auto d = src1[x * 2 + 1];

			// Red and blue accumulate in bits 0-9 and 16-25, green and alpha likewise after the
			// shift, so four samples never carry into the neighbouring channel.
			const auto rb = (a & 0x00FF00FFu) + (b & 0x00FF00FFu) + (c & 0x00FF00FFu) + (d & 0x00FF00FFu);
			const auto ga = (a >> 8 & 0x00FF00FFu) + (b >> 8 & 0x00FF00FFu) + (c >> 8 & 0x00FF00FFu) +
				(d >> 8 & 0x00FF00FFu);

			dst[x] = (rb >> 2 & 0x00FF00FFu) | ((ga >> 2 & 0x00FF00FFu) << 8);
		}
	}
}

//
// The pie is an extruded, tilted annulus. Every pixel asks the same question in depth order: the
// raised top face first, then the flat one, then the outer wall, then the well's far wall, then the
// strip a raised wedge vacated, and finally the well floor. Nothing is drawn twice and nothing is
// sorted, which is what keeps a hover re-render inside a frame.
//
void render_pie_chart(chart_surface& destination, const pie_chart_scene& scene)
{
	if (!destination.is_ready()) return;

	destination.clear();

	auto& surface = destination.supersampled();
	const auto dims = surface.dimensions();
	const auto extent = destination.extent();
	auto* const ids = destination.ids();

	const auto margin = static_cast<double>(chart_supersample);
	const auto width = static_cast<double>(dims.cx);
	const auto height = static_cast<double>(dims.cy);

	// Width usually binds; the height term keeps the rim and the lift inside the element when the
	// sidebar is wide enough for it not to.
	const auto radius = std::min((width - 2.0 * margin) / 2.0,
	                             (height - 2.0 * margin) /
	                             (2.0 * pie_chart_squash + pie_chart_rim + pie_chart_lift));

	if (radius < 4.0) return;

	const auto rim = radius * pie_chart_rim;
	const auto lift = radius * pie_chart_lift;
	const auto hole = radius * pie_chart_hole;
	const auto drawn_height = 2.0 * radius * pie_chart_squash + rim + lift;

	const auto centre_x = width / 2.0;
	const auto centre_y = (height - drawn_height) / 2.0 + lift + radius * pie_chart_squash;

	const auto radius2 = radius * radius;
	const auto hole2 = hole * hole;
	const auto segments_per_radian = pie_chart_segment_count / (2.0 * M_PI);

	// Screen-space light: chart_light_y counts up, screen y counts down.
	const auto light_x = chart_light_x;
	const auto light_y = -chart_light_y;

	const auto segment_at = [segments_per_radian](const double dx, const double dz)
	{
		const auto index = static_cast<int>((fast_atan2(dz, dx) + M_PI) * segments_per_radian);
		return std::clamp(index, 0, pie_chart_segment_count - 1);
	};

	const auto y_first = std::max(0, static_cast<int>(std::floor(centre_y - radius * pie_chart_squash - lift - 1.0)));
	const auto y_last = std::min(dims.cy - 1, static_cast<int>(std::ceil(centre_y + radius * pie_chart_squash + rim)));
	const auto x_first = std::max(0, static_cast<int>(std::floor(centre_x - radius - 1.0)));
	const auto x_last = std::min(dims.cx - 1, static_cast<int>(std::ceil(centre_x + radius)));

	for (auto y = y_first; y <= y_last; ++y)
	{
		auto* const line = std::bit_cast<ui::color32*>(surface.pixels_line(y));
		auto* const id_line = (y & 1) == 0 ? ids + static_cast<size_t>(y / chart_supersample) * extent.cx : nullptr;
		const auto py = y + 0.5 - centre_y;

		for (auto x = x_first; x <= x_last; ++x)
		{
			const auto px = x + 0.5 - centre_x;

			ui::color32 color = 0;
			auto id = chart_surface::no_id;

			// The two top faces, the raised one first because it is both higher and nearer.
			for (auto pass = 0; pass < 2 && color == 0; ++pass)
			{
				const auto want_raised = pass == 0;
				const auto dz = (py + (want_raised ? lift : 0.0)) / pie_chart_squash;
				const auto distance2 = px * px + dz * dz;

				if (distance2 > radius2 || distance2 < hole2) continue;

				const auto& wedge = scene.wedges[segment_at(px, dz)];
				if (wedge.raised != want_raised) continue;

				// A flat top face has one normal, so its only gradient is the sheen of a large soft
				// light. Without it the disc reads as paper again however deep the rim is.
				const auto toward_light = (px * light_x + dz * pie_chart_squash * light_y) / radius;
				color = shade(wedge.color, std::min(1.0, 0.90 + 0.14 * toward_light));
				id = wedge.id;
			}

			// The outer wall, on the half of the rim that faces the viewer.
			if (color == 0 && std::abs(px) <= radius)
			{
				const auto dz = std::sqrt(std::max(0.0, radius2 - px * px));
				const auto& wedge = scene.wedges[segment_at(px, dz)];
				const auto top = dz * pie_chart_squash - (wedge.raised ? lift : 0.0);

				if (py >= top && py <= dz * pie_chart_squash + rim)
				{
					const auto facing = (px * light_x + dz * light_y) / radius;
					color = shade(wedge.color, std::clamp(0.62 + 0.30 * facing, 0.26, 0.95));
					id = wedge.id;
				}
			}

			// The well's far wall. Looking into a well from above shows the side away from the
			// viewer, which is why this samples the back of the hole rather than the front.
			if (color == 0 && std::abs(px) <= hole)
			{
				const auto dz = -std::sqrt(std::max(0.0, hole2 - px * px));
				const auto& wedge = scene.wedges[segment_at(px, dz)];
				const auto top = dz * pie_chart_squash - (wedge.raised ? lift : 0.0);

				if (py >= top && py <= dz * pie_chart_squash + rim)
				{
					color = shade(wedge.color, 0.34);
					id = wedge.id;
				}
			}

			// What a raised wedge left behind is its own cut face, so the lift shows a wall instead
			// of a hole through the chart.
			if (color == 0)
			{
				const auto dz = py / pie_chart_squash;
				const auto distance2 = px * px + dz * dz;

				if (distance2 <= radius2 && distance2 >= hole2)
				{
					const auto& wedge = scene.wedges[segment_at(px, dz)];

					if (wedge.raised)
					{
						color = shade(wedge.color, 0.48);
						id = wedge.id;
					}
				}
			}

			if (color == 0)
			{
				const auto dz = (py - rim) / pie_chart_squash;

				if (px * px + dz * dz <= hole2)
				{
					color = 0xFF000000u | (scene.hole_color & 0x00FFFFFFu);
					id = scene.hole_id;
				}
			}

			if (color == 0) continue;

			line[x] = color;

			if (id_line && (x & 1) == 0) id_line[x / chart_supersample] = id;
		}
	}

	destination.reduce();
}

//
// The calendar is a field of shallow blocks rather than one receding isometric city: a city hides
// its short bars behind its tall ones, and hidden data cannot be hovered or clicked. A tall block
// may clear its own cell and lean over the row above, which the caller bounds so that every month
// keeps a band of its own top exposed. Cells arrive in drawing order, so the caller decides what
// occludes what.
//
void render_calendar_chart(chart_surface& destination, const std::vector<calendar_chart_cell>& cells,
                           const calendar_chart_style& style, const bool restart)
{
	if (!destination.is_ready()) return;

	if (restart) destination.clear();

	auto& surface = destination.supersampled();
	const auto extent = destination.extent();
	auto* const ids = destination.ids();

	const auto scale = chart_supersample;
	const auto depth_x = (style.depth_x == 0 ? 1 : style.depth_x) * scale;
	const auto depth_y = std::max(1, style.depth_y) * scale;
	const auto gap = std::max(1, style.gap) * scale;
	const auto lift = std::max(0, style.lift) * scale;

	// Whichever way the depth leans, the block on that side is the further one, so the caller hands
	// cells over in drawing order and the nearer block simply arrives last.
	for (const auto& cell : cells)
	{
		const auto left = cell.cell.left * scale + std::max(0, -depth_x);
		const auto right = cell.cell.right * scale - gap - std::max(0, depth_x);
		if (right - left < scale) continue;

		const auto floor_y = cell.cell.bottom * scale - scale - (cell.raised ? lift : 0);
		const auto block_height = std::max(0, cell.height) * scale;
		const auto top_y = floor_y - block_height;

		// Three clearly separate tones. Light from the upper left: the cap catches it and the face
		// being looked at sits in the middle. The visible side turns away from the light when the
		// scene recedes right and into it when it recedes left, so its tone follows the sign.
		const auto top_face = ui::lighten(cell.color, 0.34f);
		const auto front_face = ui::darken(cell.color, 0.10f);
		const auto side_face = depth_x > 0 ? ui::darken(cell.color, 0.52f) : ui::lighten(cell.color, 0.18f);
		const auto side_edge = depth_x > 0 ? right : left;

		// The top face alone is the empty socket an unphotographed month leaves. Absence has to be
		// visible or the grid stops reading as a calendar.
		fill_convex(surface, ids, extent, {
			            pointi(left, top_y), pointi(left + depth_x, top_y - depth_y),
			            pointi(right + depth_x, top_y - depth_y), pointi(right, top_y)
		            }, shade(top_face, 1.0), cell.id);

		if (block_height > 0)
		{
			fill_convex(surface, ids, extent, {
				            pointi(side_edge, top_y), pointi(side_edge + depth_x, top_y - depth_y),
				            pointi(side_edge + depth_x, floor_y - depth_y), pointi(side_edge, floor_y)
			            }, shade(side_face, 1.0), cell.id);

			fill_convex(surface, ids, extent, {
				            pointi(left, top_y), pointi(right, top_y),
				            pointi(right, floor_y), pointi(left, floor_y)
			            }, shade(front_face, 1.0), cell.id);
		}
	}

	destination.reduce();
}
