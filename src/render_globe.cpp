// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Software rasteriser for the sidebar globe. Resamples an equirectangular world map
// onto an orthographic sphere for a given view, into a surface the caller uploads as a texture.
// Backend-independent by construction: the pixels are the same under D3D11 and CPU rendering.

#include "pch.h"
#include "ui_globe.h"

// Pixels between exact evaluations of the sphere inverse. Everything between is interpolated in
// source texel space, which is what keeps a drag affordable: the transcendentals cost a sixteenth
// of what a per-pixel inverse would.
static constexpr int globe_segment_px = 16;

// Interpolating longitude across a segment stops being honest this close to a pole, where a step
// of one pixel can swing it most of the way round. Those segments are evaluated per pixel.
static constexpr double globe_pole_guard_rad = 1.396; // 80 degrees

// Light direction in the view plane. Keeping it out of the z axis makes the diffuse term linear in
// the pixel offset, so it costs a multiply rather than a square root.
static constexpr double globe_light_x = -0.55;
static constexpr double globe_light_y = 0.65;

namespace
{
	struct sample_point
	{
		double u = 0.0; // source column, in level-0 texels
		double v = 0.0; // source row, in level-0 texels
		double longitude = 0.0;
	};

	// The reduced copy a segment reads from, as raw pointers: this is the inner loop, and going
	// through the surface accessors per pixel costs more than the sampling does.
	struct source_level
	{
		const uint8_t* pixels = nullptr;
		size_t stride = 0;
		int cx = 0;
		int cy = 0;
	};

	// Scales the three colour bytes by a Q8 factor without caring which of them is red. Alpha is
	// the caller's answer, not the source's.
	inline ui::color32 modulate(const ui::color32 c, const uint32_t shade_q8, const uint32_t alpha)
	{
		const auto rb = (((c & 0x00FF00FFu) * shade_q8) >> 8) & 0x00FF00FFu;
		const auto g = (((c & 0x0000FF00u) * shade_q8) >> 8) & 0x0000FF00u;
		return (alpha << 24) | rb | g;
	}

	// Two colours blended with a Q8 weight, a channel pair at a time. The weights sum to 256, so
	// each 16 bit lane peaks at 255*256 and nothing carries into its neighbour.
	inline ui::color32 blend_q8(const ui::color32 c1, const ui::color32 c2, const uint32_t t)
	{
		const auto inverse = 256u - t;
		const auto rb = ((((c1 & 0x00FF00FFu) * inverse) + ((c2 & 0x00FF00FFu) * t)) >> 8) & 0x00FF00FFu;
		const auto ga = (((c1 >> 8 & 0x00FF00FFu) * inverse) + ((c2 >> 8 & 0x00FF00FFu) * t)) & 0xFF00FF00u;
		return rb | ga;
	}

	inline ui::color32 sample_bilinear(const source_level& level, const double u, const double v)
	{
		const auto u_floor = std::floor(u);
		const auto v_floor = std::floor(v);
		const auto u_frac = static_cast<uint32_t>((u - u_floor) * 256.0) & 0xFFu;
		const auto v_frac = static_cast<uint32_t>((v - v_floor) * 256.0) & 0xFFu;

		// The map wraps in longitude and stops at the poles.
		auto x0 = static_cast<int>(u_floor) % level.cx;
		if (x0 < 0) x0 += level.cx;
		const auto x1 = x0 + 1 == level.cx ? 0 : x0 + 1;
		const auto y0 = std::clamp(static_cast<int>(v_floor), 0, level.cy - 1);
		const auto y1 = std::min(y0 + 1, level.cy - 1);

		const auto line0 = std::bit_cast<const ui::color32*>(level.pixels + y0 * level.stride);
		const auto line1 = std::bit_cast<const ui::color32*>(level.pixels + y1 * level.stride);

		return blend_q8(blend_q8(line0[x0], line0[x1], u_frac), blend_q8(line1[x0], line1[x1], u_frac), v_frac);
	}
}

void globe_renderer::set_source(ui::const_surface_ptr source)
{
	_levels.clear();

	if (!is_valid(source)) return;

	_levels.emplace_back(source);
	auto extent = source->dimensions();

	// Reduced copies pay for themselves at the limb, where a screen pixel can span dozens of
	// source texels and point sampling crawls while the user drags.
	while (extent.cx > 64 && extent.cy > 32)
	{
		const sizei next(extent.cx / 2, extent.cy / 2);
		ui::surface_ptr reduced;
		if (!ui::area_downscale(_levels.back(), reduced, next)) break;
		_levels.emplace_back(std::move(reduced));
		extent = next;
	}
}

bool globe_renderer::render(ui::surface& destination, const globe_projection& projection) const
{
	if (_levels.empty()) return false;

	const auto radius = projection.radius();
	const auto center = projection.center();
	const auto dims = destination.dimensions();

	if (radius < 2.0 || dims.cx < 1 || dims.cy < 1) return false;

	// A failed alloc reports the extent it was asked for and holds no memory, so the size is not the
	// question - whether the surface actually has pixels is.
	if (destination.empty()) return false;

	const auto& source = *_levels.front();
	const auto source_cx = static_cast<double>(source.dimensions().cx);
	const auto source_cy = static_cast<double>(source.dimensions().cy);
	const auto max_level = static_cast<int>(_levels.size()) - 1;

	memset(destination.pixels(), 0, destination.size());

	const auto radius_px = static_cast<int>(std::floor(radius));
	const auto y_first = std::max(0, center.y - radius_px);
	const auto y_last = std::min(dims.cy - 1, center.y + radius_px);

	// Beyond this the edge pixel is only partly covered by the sphere, so it earns partial alpha
	// rather than a hard staircase.
	const auto feather_from = (radius - 1.5) * (radius - 1.5) / (radius * radius);

	const auto to_sample = [&](const double ux, const double uy, sample_point& out, double& latitude)
	{
		double lat, lon;
		if (!projection.unproject_unit(ux, uy, lat, lon)) return false;
		latitude = lat;
		out.longitude = lon;
		out.u = (lon / (2.0 * M_PI) + 0.5) * source_cx;
		out.v = (0.5 - lat / M_PI) * source_cy;
		return true;
	};

	for (auto y = y_first; y <= y_last; ++y)
	{
		const auto uy = (center.y - y) / radius;
		const auto row_half = 1.0 - uy * uy;
		if (row_half <= 0.0) continue;

		const auto half_width = std::sqrt(row_half);
		const auto x_first = std::max(0, static_cast<int>(std::ceil(center.x - half_width * radius)));
		const auto x_last = std::min(dims.cx - 1, static_cast<int>(std::floor(center.x + half_width * radius)));
		if (x_first > x_last) continue;

		auto* const line = std::bit_cast<ui::color32*>(destination.pixels_line(y));
		const auto uy2 = uy * uy;

		for (auto x = x_first; x <= x_last;)
		{
			const auto x_end = std::min(x + globe_segment_px, x_last);
			const auto span = x_end - x;

			const auto ux_at = [&](const int px)
			{
				return std::clamp((px - center.x) / radius, -half_width, half_width);
			};

			sample_point begin, end;
			double lat_begin = 0.0, lat_end = 0.0;

			if (!to_sample(ux_at(x), uy, begin, lat_begin) || !to_sample(ux_at(x_end), uy, end, lat_end))
			{
				x = x_end + 1;
				continue;
			}

			// Unwrap so a segment straddling the date line interpolates through it instead of
			// sweeping the whole map backwards.
			auto end_longitude = end.longitude;
			while (end_longitude - begin.longitude > M_PI) end_longitude -= 2.0 * M_PI;
			while (end_longitude - begin.longitude < -M_PI) end_longitude += 2.0 * M_PI;
			end.u = (end_longitude / (2.0 * M_PI) + 0.5) * source_cx;

			const auto exact = span == 0 ||
				std::abs(lat_begin) > globe_pole_guard_rad || std::abs(lat_end) > globe_pole_guard_rad;

			// How much of the map one screen pixel covers here chooses the reduced copy to read.
			auto level_index = 0;

			if (span > 0)
			{
				const auto texels_per_pixel = std::max(
					std::abs(end.u - begin.u), std::abs(end.v - begin.v)) / span;
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
			const auto level_scale = static_cast<double>(texels.cx) / source_cx;
			const auto level_scale_y = static_cast<double>(texels.cy) / source_cy;

			for (auto px = x; px <= x_end; ++px)
			{
				const auto ux = ux_at(px);
				sample_point at = begin;

				if (exact)
				{
					double lat;
					if (!to_sample(ux, uy, at, lat)) continue;
				}
				else
				{
					const auto t = static_cast<double>(px - x) / span;
					at.u = begin.u + (end.u - begin.u) * t;
					at.v = begin.v + (end.v - begin.v) * t;
				}

				const auto r2 = ux * ux + uy2;
				const auto diffuse = ux * globe_light_x + uy * globe_light_y;

				// Never brighter than the map itself: the Q8 modulate has no headroom above 1.0,
				// and a globe that only darkens keeps the map's own colours where the light is.
				const auto shade = std::clamp((1.0 + 0.55 * diffuse) * (1.0 - 0.25 * r2 * r2), 0.18, 1.0);

				auto shade_q8 = static_cast<uint32_t>(shade * 256.0);
				auto alpha = 255u;

				if (r2 > feather_from)
				{
					const auto coverage = (1.0 - std::sqrt(r2)) * radius + 0.5;
					const auto covered = df::round(coverage * 255.0);
					if (covered <= 0) continue;

					alpha = static_cast<uint32_t>(std::min(covered, 255));

					// Premultiplied at the limb, matching what the chart rasterisers do where their
					// silhouettes fade to nothing.
					shade_q8 = shade_q8 * alpha / 255u;
				}

				line[px] = modulate(sample_bilinear(texels, at.u * level_scale, at.v * level_scale_y),
				                    shade_q8, alpha);
			}

			x = x_end + 1;
		}
	}

	return true;
}
