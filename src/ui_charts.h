// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Geometry and material shared by the sidebar's software-rendered charts - the file-type
// pie and the history calendar. The rasterisers declared here are implemented in render_charts.cpp.
//
// The globe is not merely round, it is lit: render_globe.cpp shades every pixel from one fixed
// light and darkens toward the limb, so it reads as a solid object. A flat disc and a flat heat
// grid beside it read as diagrams of one. These charts therefore borrow the globe's light vector
// and its shading ramp rather than inventing their own, because what makes the three look like one
// panel is the shared material, not the extrusion.
//
// Every rasteriser writes two answers: the pixels, and a 1x buffer naming what each pixel shows.
// Hit testing reads that buffer. Once a chart is tilted and extruded, no rect or angle recomputed
// afterwards agrees with the silhouette the user is aiming at - the wedge under the pointer is not
// the wedge atan2 names, and a block that has risen no longer fills its cell. design.md targeting
// asks that the pointer be ON the thing it selects, and an identity buffer is the only cheap way
// to mean that exactly.

#pragma once

#include "libdesktop/ui.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// The globe's light, in view-plane units with y up. Keeping the charts on the same vector is what
// makes them share a panel; a chart lit from elsewhere reads as a sticker on the sidebar.
inline constexpr double chart_light_x = -0.55;
inline constexpr double chart_light_y = 0.65;

// Charts are rasterised at this multiple and box-reduced, which is what gives the silhouettes their
// edges. The identity buffer stays at 1x, because that is the resolution the pointer is in.
inline constexpr int chart_supersample = 2;

// Vertical squash of the pie's disc: the ellipse is this fraction as tall as it is wide. Deeper
// tilts look more dramatic and crush the smaller media types into slivers nobody can aim at.
inline constexpr double pie_chart_squash = 0.72;

// The extruded rim and the hover lift, as fractions of the radius.
inline constexpr double pie_chart_rim = 0.14;
inline constexpr double pie_chart_lift = 0.055;

// The hole, as a fraction of the radius. It is a well rather than a gap because the collection
// total is drawn in it, and text needs a floor of its own to stay legible.
inline constexpr double pie_chart_hole = 0.56;

inline constexpr int pie_chart_segment_count = 64;

// The disc plus its rim and its hover lift, as a fraction of the width. A tilted pie is wider than
// it is tall, and a square box would leave a fifth of itself empty above and below the chart.
inline constexpr double pie_chart_aspect = (2.0 * pie_chart_squash + pie_chart_rim + pie_chart_lift) / 2.0;

// Identities the pie's buffer records. Zero is "nothing", so the hole and the wedges start at one.
inline constexpr uint16_t pie_chart_hole_id = 1;
inline constexpr uint16_t pie_chart_wedge_id_base = 2;

// Month buckets span orders of magnitude - a holiday month against a month nobody photographed -
// so block height is logarithmic. A month with ten times the items is not ten times as tall; the
// tooltip stays the source of the exact count, which is what keeps the compression honest.
inline double chart_log_height(const uint64_t count, const uint64_t max_count)
{
	if (count == 0 || max_count == 0) return 0.0;
	return std::log1p(static_cast<double>(count)) / std::log1p(static_cast<double>(max_count));
}

// A rasterised chart: the pixels to upload, and what the pointer would be pointing at. The buffers
// are kept between renders because hover re-rasterises, and allocating per pointer move would put
// an allocation behind moving the mouse.
class chart_surface
{
	ui::surface_ptr _pixels; // 1x ARGB, premultiplied at the silhouette by the reduction
	ui::surface_ptr _scratch; // the supersampled buffer the rasterisers draw into
	std::vector<uint16_t> _ids; // 1x, one identity per pixel
	sizei _extent;

public:
	static constexpr uint16_t no_id = 0;

	bool prepare(sizei extent);
	// The buffers, not the pointers: surface::alloc answers null pixels on a failed allocation while
	// still reporting a size, so testing the shared_ptr alone lets a memset run against nothing.
	bool is_ready() const { return _extent.cx > 0 && ui::is_valid(_pixels) && ui::is_valid(_scratch); }
	sizei extent() const { return _extent; }

	const ui::surface_ptr& pixels() const { return _pixels; }

	uint16_t id_at(const pointi at) const
	{
		if (at.x < 0 || at.y < 0 || at.x >= _extent.cx || at.y >= _extent.cy) return no_id;
		return _ids[static_cast<size_t>(at.y) * _extent.cx + at.x];
	}

	// The rasterisers draw into these, then reduce.
	ui::surface& supersampled() const { return *_scratch; }
	uint16_t* ids() { return _ids.data(); }
	void clear();
	void reduce() const;
};

struct pie_chart_wedge
{
	ui::color32 color = 0; // in the surface's channel order
	uint16_t id = chart_surface::no_id;
	bool raised = false;
};

struct pie_chart_scene
{
	std::array<pie_chart_wedge, pie_chart_segment_count> wedges{};

	// The well's floor. Always opaque: a transparent floor would let the sidebar show through a
	// solid object, and would leave the centre - which opens collection options - with no pixels
	// of its own to be pointed at.
	ui::color32 hole_color = 0;
	uint16_t hole_id = pie_chart_hole_id;
};

void render_pie_chart(chart_surface& destination, const pie_chart_scene& scene);

struct calendar_chart_cell
{
	recti cell; // the 1x rect the month owns
	int height = 0; // 1x pixels above the cell's floor line; the caller may let it clear the cell
	ui::color32 color = 0;
	uint16_t id = chart_surface::no_id;
	bool raised = false;
};

struct calendar_chart_style
{
	// Signed. A positive depth recedes up and to the right and shows the block's right face; a
	// negative one recedes up and to the left and shows its left face. Whoever lays the cells out
	// must skew the rows the same way, or the chart has two directions of depth at once.
	int depth_x = 4;
	int depth_y = 5;
	int gap = 2; // the clear space between months; the faces stop short of the cell edge by this
	int lift = 2; // how far a hovered block floats
};

// `restart` false draws over what is already there instead of clearing, so a caller can lay two
// bands with different depths - a calendar and the navigator under it - into one surface.
void render_calendar_chart(chart_surface& destination, const std::vector<calendar_chart_cell>& cells,
                           const calendar_chart_style& style, bool restart = true);
