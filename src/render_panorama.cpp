// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Software rasteriser for a projected equirectangular panorama. Resamples the file's
// pixels through the viewer's camera for a given view, into a surface the caller uploads as a
// texture. Backend-independent by construction: the pixels are the same under D3D11 and CPU
// rendering.

#include "pch.h"
#include "ui_panorama.h"

// Pixels between exact evaluations of the camera inverse. Everything between is interpolated in
// source texel space, which is what keeps a drag affordable: a per-pixel inverse costs three
// transcendentals, and a full viewport of them is not a frame budget.
static constexpr int panorama_segment_px = 8;

// Interpolating longitude across a segment stops being honest this close to a pole, where a step
// of one pixel can swing it most of the way round. Those segments are evaluated per pixel.
static constexpr double panorama_pole_guard_rad = 1.483; // 85 degrees

namespace
{
	// The reduced copy a segment reads from, as raw pointers: this is the inner loop, and going
	// through the surface accessors per pixel costs more than the sampling does.
	struct source_level
	{
		const uint8_t* pixels = nullptr;
		size_t stride = 0;
		int cx = 0;
		int cy = 0;
	};

	// Two colours blended with a Q8 weight, a channel pair at a time. The weights sum to 256, so
	// each 16 bit lane peaks at 255*256 and nothing carries into its neighbour.
	inline ui::color32 blend_q8(const ui::color32 c1, const ui::color32 c2, const uint32_t t)
	{
		const auto inverse = 256u - t;
		const auto rb = ((((c1 & 0x00FF00FFu) * inverse) + ((c2 & 0x00FF00FFu) * t)) >> 8) & 0x00FF00FFu;
		const auto ga = (((c1 >> 8 & 0x00FF00FFu) * inverse) + ((c2 >> 8 & 0x00FF00FFu) * t)) & 0xFF00FF00u;
		return rb | ga;
	}

	inline ui::color32 sample_bilinear(const source_level& level, double u, double v, const bool wrap)
	{
		// Clamped before the fraction is taken, not after: a coordinate half a texel outside the top
		// or left edge otherwise keeps a fraction that weights it toward the *second* row, turning an
		// edge into an inverted smear.
		if (!wrap) u = std::clamp(u, 0.0, static_cast<double>(level.cx - 1));
		v = std::clamp(v, 0.0, static_cast<double>(level.cy - 1));

		const auto u_floor = std::floor(u);
		const auto v_floor = std::floor(v);
		const auto u_frac = static_cast<uint32_t>((u - u_floor) * 256.0) & 0xFFu;
		const auto v_frac = static_cast<uint32_t>((v - v_floor) * 256.0) & 0xFFu;

		int x0, x1;

		if (wrap)
		{
			x0 = static_cast<int>(u_floor) % level.cx;
			if (x0 < 0) x0 += level.cx;
			x1 = x0 + 1 == level.cx ? 0 : x0 + 1;
		}
		else
		{
			x0 = std::clamp(static_cast<int>(u_floor), 0, level.cx - 1);
			x1 = std::min(x0 + 1, level.cx - 1);
		}

		const auto y0 = std::clamp(static_cast<int>(v_floor), 0, level.cy - 1);
		const auto y1 = std::min(y0 + 1, level.cy - 1);

		const auto line0 = std::bit_cast<const ui::color32*>(level.pixels + y0 * level.stride);
		const auto line1 = std::bit_cast<const ui::color32*>(level.pixels + y1 * level.stride);

		return blend_q8(blend_q8(line0[x0], line0[x1], u_frac), blend_q8(line1[x0], line1[x1], u_frac), v_frac);
	}
}

void panorama_renderer::set_source(ui::const_surface_ptr source)
{
	_levels.clear();

	if (!is_valid(source)) return;

	// Planar YUV walked as colour values is grey and tiled, and it fails silently: it looks like a
	// broken projection rather than like a surface the reader cannot read. The decode is asked for
	// packed pixels when a projection will consume them, so refusing here holds that to its word
	// instead of guessing at a conversion.
	if (!ui::is_packed(source->format())) return;

	_levels.emplace_back(std::move(source));
	auto extent = _levels.front()->dimensions();

	// Reduced copies pay for themselves at a wide field of view, where a screen pixel can span
	// dozens of source texels and point sampling crawls while the user drags. Either axis keeps the
	// ladder going: a 12000 x 1500 strip needs the width reduced long after the height has run out,
	// and stopping at the short axis leaves the wide end of the ladder aliasing.
	while (extent.cx > 64 || extent.cy > 32)
	{
		const sizei next(std::max(1, extent.cx / 2), std::max(1, extent.cy / 2));
		if (next.cx == extent.cx && next.cy == extent.cy) break;

		ui::surface_ptr reduced;
		if (!ui::area_downscale(_levels.back(), reduced, next)) break;
		_levels.emplace_back(std::move(reduced));
		extent = next;
	}
}

sizei panorama_renderer::source_extent() const
{
	return _levels.empty() ? sizei{} : _levels.front()->dimensions();
}

bool panorama_renderer::render(ui::surface& destination, const prop::panorama_geometry& geometry,
                               const panorama_view& view) const
{
	if (_levels.empty() || !geometry.is_valid()) return false;

	const auto dims = destination.dimensions();

	if (dims.cx < 1 || dims.cy < 1) return false;

	// A failed alloc reports the extent it was asked for and holds no memory, so the size is not the
	// question - whether the surface actually has pixels is.
	if (destination.empty()) return false;

	const auto source_extent = _levels.front()->dimensions();
	const auto source_cx = static_cast<double>(source_extent.cx);
	const auto source_cy = static_cast<double>(source_extent.cy);

	// The geometry is expressed in the file's own pixels; the decode is whatever size the budget
	// allowed. One factor carries between the two, so nothing below has to know which is which.
	const auto texel_scale_x = source_cx / std::max(1, geometry.cropped_width);
	const auto texel_scale_y = source_cy / std::max(1, geometry.cropped_height);

	const auto max_level = static_cast<int>(_levels.size()) - 1;
	const auto wraps = panorama_wraps_longitude(geometry);
	const auto viewport = sized(dims);

	// Sphere the file does not hold reads as absence rather than as a colour it might be mistaken
	// for. A partial panorama is meant to end.
	memset(destination.pixels(), 0, destination.size());

	const auto full_width = static_cast<double>(geometry.full_width);

	// Taken once for the whole frame: yaw, pitch and field of view are fixed for the duration of a
	// render, and the five transcendentals they imply are what would otherwise be paid three times
	// per segment.
	const auto camera = view.camera_basis();

	const auto sample_at = [&](const double x, const double y, double& u, double& v, double& latitude)
	{
		double dx, dy, dz;
		view.direction(x, y, viewport, camera, dx, dy, dz);
		latitude = std::asin(std::clamp(dy, -1.0, 1.0));
		panorama_texel_at(geometry, std::atan2(dx, dz), latitude, u, v);
	};

	// panorama_texel_at wraps longitude into the file, so two ends of one segment can sit either
	// side of the seam and read as a jump across the whole width. Both the interpolation and the
	// minification rate want the short arc - the same unwrap the globe does across the date line,
	// and without it a segment on the seam reads as hundreds of texels per pixel and collapses to
	// the coarsest reduced copy.
	const auto unwrap = [full_width](const double from, double to)
	{
		while (to - from > full_width / 2.0) to -= full_width;
		while (to - from < -full_width / 2.0) to += full_width;
		return to;
	};

	const auto covers = [&](double& u, const double v)
	{
		if (wraps)
		{
			while (u < 0.0) u += full_width;
			while (u >= full_width) u -= full_width;

			// Every longitude is covered by a file that closes the circle, including the pixel or two
			// a writer rounded away - the sampler wraps across the join rather than stopping at it.
			return v >= 0.0 && v < geometry.cropped_height;
		}

		return u >= 0.0 && u < geometry.cropped_width && v >= 0.0 && v < geometry.cropped_height;
	};

	for (auto y = 0; y < dims.cy; ++y)
	{
		auto* const line = std::bit_cast<ui::color32*>(destination.pixels_line(y));
		const auto sample_y = y + 0.5;

		for (auto x = 0; x < dims.cx;)
		{
			const auto x_end = std::min(x + panorama_segment_px, dims.cx - 1);
			const auto span = x_end - x;

			double begin_u, begin_v, begin_lat, end_u, end_v, end_lat;
			sample_at(x + 0.5, sample_y, begin_u, begin_v, begin_lat);
			sample_at(x_end + 0.5, sample_y, end_u, end_v, end_lat);

			const auto unwrapped_end_u = unwrap(begin_u, end_u);

			// Longitude swings arbitrarily fast this close to a pole, so a straight line between two
			// endpoints is simply wrong there.
			const auto exact = span == 0 ||
				std::abs(begin_lat) > panorama_pole_guard_rad || std::abs(end_lat) > panorama_pole_guard_rad;

			auto level_index = 0;

			if (span > 0)
			{
				// Both columns of the Jacobian. Measuring only the rate along the segment under-
				// filters the left and right of a wide view, where one destination pixel is taller in
				// source texels than it is wide, and that reads as shimmer while the user drags.
				double down_u, down_v, down_lat;
				sample_at(x + 0.5, sample_y + 1.0, down_u, down_v, down_lat);

				const auto texels_per_pixel = std::max({
					std::abs(unwrapped_end_u - begin_u) * texel_scale_x / span,
					std::abs(end_v - begin_v) * texel_scale_y / span,
					std::abs(unwrap(begin_u, down_u) - begin_u) * texel_scale_x,
					std::abs(down_v - begin_v) * texel_scale_y
				});

				if (texels_per_pixel > 1.0)
				{
					level_index = std::clamp(static_cast<int>(std::log2(texels_per_pixel)), 0, max_level);
				}
			}

			const auto& level = *_levels[level_index];
			const source_level texels{
				level.pixels(), static_cast<size_t>(level.stride()),
				level.dimensions().cx, level.dimensions().cy
			};
			const auto level_scale_x = texels.cx * texel_scale_x / source_cx;
			const auto level_scale_y = texels.cy * texel_scale_y / source_cy;

			for (auto px = x; px <= x_end; ++px)
			{
				auto u = begin_u;
				auto v = begin_v;

				if (exact)
				{
					double latitude;
					sample_at(px + 0.5, sample_y, u, v, latitude);
				}
				else
				{
					const auto t = static_cast<double>(px - x) / span;
					u = begin_u + (unwrapped_end_u - begin_u) * t;
					v = begin_v + (end_v - begin_v) * t;
				}

				// Decided per pixel rather than per segment. A screen row is a curve in latitude, so
				// both endpoints can sit inside a partial panorama's band while the middle leaves it,
				// and both can sit outside while the middle is inside - one paints across the edge of
				// the data, the other bites lumps out of it.
				if (!covers(u, v)) continue;

				// Half a texel back: u and v are distances from the edge of texel zero, and the
				// sampler reads them as texel centres. Uncorrected it is a half-texel shift that
				// changes size with the reduced copy in force, so it shows as a tear wherever two
				// segments choose different levels.
				line[px] = sample_bilinear(texels, u * level_scale_x - 0.5, v * level_scale_y - 0.5, wraps)
					| 0xFF000000u;
			}

			x = x_end + 1;
		}
	}

	return true;
}
