// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Software rasteriser for the sidebar's and Explore's globes. Resamples an equirectangular
// world map onto an orthographic sphere for a given view, draping map tiles over it where Explore
// has them, into a surface the caller uploads as a texture. Backend-independent by construction: the
// pixels are the same under D3D11 and CPU rendering.

#include "pch.h"
#include "ui_globe.h"

#ifdef _MSC_VER
#include <execution>
#endif

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
		double my = 0.0; // Web Mercator y, as a fraction of the world from its north edge
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
	inline core::color32 modulate(const core::color32 c, const uint32_t shade_q8, const uint32_t alpha)
	{
		const auto rb = (((c & 0x00FF00FFu) * shade_q8) >> 8) & 0x00FF00FFu;
		const auto g = (((c & 0x0000FF00u) * shade_q8) >> 8) & 0x0000FF00u;
		return (alpha << 24) | rb | g;
	}

	// Two colours blended with a Q8 weight, a channel pair at a time. The weights sum to 256, so
	// each 16 bit lane peaks at 255*256 and nothing carries into its neighbour.
	inline core::color32 blend_q8(const core::color32 c1, const core::color32 c2, const uint32_t t)
	{
		const auto inverse = 256u - t;
		const auto rb = ((((c1 & 0x00FF00FFu) * inverse) + ((c2 & 0x00FF00FFu) * t)) >> 8) & 0x00FF00FFu;
		const auto ga = (((c1 >> 8 & 0x00FF00FFu) * inverse) + ((c2 >> 8 & 0x00FF00FFu) * t)) & 0xFF00FF00u;
		return rb | ga;
	}

	inline core::color32 sample_bilinear(const source_level& level, const double u, const double v)
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

		const auto line0 = std::bit_cast<const core::color32*>(level.pixels + y0 * level.stride);
		const auto line1 = std::bit_cast<const core::color32*>(level.pixels + y1 * level.stride);

		return blend_q8(blend_q8(line0[x0], line0[x1], u_frac), blend_q8(line1[x0], line1[x1], u_frac), v_frac);
	}

	// A tile is sampled at its texel centres and held at its own edge: its neighbour may not be loaded, and
	// the edge texel is the nearer colour anyway.
	inline core::color32 sample_tile(const core::surface& tile, const double u, const double v)
	{
		const auto dims = tile.dimensions();
		const auto x = std::clamp(u - 0.5, 0.0, static_cast<double>(dims.cx - 1));
		const auto y = std::clamp(v - 0.5, 0.0, static_cast<double>(dims.cy - 1));
		const auto x0 = static_cast<int>(x);
		const auto y0 = static_cast<int>(y);
		const auto x1 = std::min(x0 + 1, dims.cx - 1);
		const auto y1 = std::min(y0 + 1, dims.cy - 1);
		const auto x_frac = static_cast<uint32_t>((x - x0) * 256.0) & 0xFFu;
		const auto y_frac = static_cast<uint32_t>((y - y0) * 256.0) & 0xFFu;

		const auto line0 = std::bit_cast<const core::color32*>(tile.pixels_line(y0));
		const auto line1 = std::bit_cast<const core::color32*>(tile.pixels_line(y1));

		return blend_q8(blend_q8(line0[x0], line0[x1], x_frac), blend_q8(line1[x0], line1[x1], x_frac), y_frac) |
		       0xFF000000u;
	}

	// Web Mercator's y for a latitude, as a fraction of the world from its north edge. Outside [0, 1) past
	// the latitudes the projection covers.
	inline double mercator_y(const double latitude_rad)
	{
		return (1.0 - std::asinh(std::tan(latitude_rad)) / M_PI) / 2.0;
	}

	// A tile address, as the source is asked for one.
	struct tile_address
	{
		int z = 0;
		int x = 0;
		int y = 0;
	};

	// What is drawn for a tile: the tile itself or, while it is on its way, the tiles a level finer still held
	// from before a zoom out, each over its quarter, and the nearest ancestor held over the rest.
	struct tile_stand_in
	{
		int z = -1;
		int x = 0;
		int y = 0;
		core::const_surface_ptr surface; // the tile, or the ancestor `up` levels above it
		int up = 0;
		std::array<core::const_surface_ptr, 4> quarters; // left to right, then top to bottom
	};

	// The tiles a band of rows reads. Finding one costs a lock and a lookup the inner loop cannot pay per
	// pixel, and a band crosses the same tiles row after row, so those found are kept, a slot for each
	// address. What the band wanted - each tile it reached, and each held one standing in, so it is not let
	// go meanwhile - is collected for the source, which is told once the band is drawn.
	class tile_reader
	{
		globe_tile_source* _source = nullptr;
		std::array<tile_stand_in, 64> _found;
		size_t _current = 0;
		std::vector<tile_address> _wanted;

		size_t resolve(const int z, const int x, const int y)
		{
			const auto i = (static_cast<size_t>(x) * 73856093u ^ static_cast<size_t>(y) * 19349663u ^
			                static_cast<size_t>(z) * 83492791u) %
			               _found.size();
			auto& r = _found[i];
			if (r.z == z && r.x == x && r.y == y) return i;

			r = {z, x, y};
			_wanted.push_back({z, x, y});

			if (auto found = _source->tile(z, x, y); is_valid(found))
			{
				r.surface = std::move(found);
				return i;
			}

			if (z < globe_max_tile_level)
			{
				for (auto q = 0; q < 4; ++q)
				{
					const tile_address finer{z + 1, x * 2 + (q & 1), y * 2 + (q >> 1)};

					if (auto found = _source->tile(finer.z, finer.x, finer.y); is_valid(found))
					{
						r.quarters[q] = std::move(found);
						_wanted.push_back(finer);
					}
				}

				if (std::ranges::all_of(r.quarters, [](const auto& s) { return s != nullptr; })) return i;
			}

			for (auto up = 1; up <= z; ++up)
			{
				if (auto found = _source->tile(z - up, x >> up, y >> up); is_valid(found))
				{
					r.surface = std::move(found);
					r.up = up;
					_wanted.push_back({z - up, x >> up, y >> up});
					break;
				}
			}

			return i;
		}

		// A point within a tile, as fractions of its width and height.
		static core::color32 sample_within(const core::surface& tile, const double fx, const double fy)
		{
			const auto dims = tile.dimensions();
			return sample_tile(tile, fx * dims.cx, fy * dims.cy);
		}

	public:
		explicit tile_reader(globe_tile_source* source) : _source(source) {}

		const std::vector<tile_address>& wanted() const { return _wanted; }

		// The colour at a Web Mercator point, read at level z, or nothing when no loaded tile covers it.
		std::optional<core::color32> sample(const int z, const double mx, const double my)
		{
			if (my < 0.0 || my >= 1.0) return {};

			const auto tiles = 1 << z;
			const auto tx = (mx - std::floor(mx)) * tiles;
			const auto ty = my * tiles;
			const auto x = std::min(static_cast<int>(tx), tiles - 1);
			const auto y = std::min(static_cast<int>(ty), tiles - 1);

			if (const auto& c = _found[_current]; c.z != z || c.x != x || c.y != y) _current = resolve(z, x, y);
			const auto& r = _found[_current];
			const auto fx = tx - x;
			const auto fy = ty - y;

			if (r.surface && r.up == 0) return sample_within(*r.surface, fx, fy);

			const auto q = (fx >= 0.5 ? 1 : 0) + (fy >= 0.5 ? 2 : 0);
			if (r.quarters[q]) return sample_within(*r.quarters[q], fx * 2.0 - (q & 1), fy * 2.0 - (q >> 1));

			if (!r.surface) return {};

			// The ancestor's part covering this tile: a 2^up-th of it, where the tile sits within it.
			const auto scale = 1 << r.up;
			return sample_within(*r.surface, ((x & (scale - 1)) + fx) / scale, ((y & (scale - 1)) + fy) / scale);
		}
	};
}

// The map ships as flat-toned land over transparent water, so drawing it as it comes left the oceans
// the colour of whatever was behind them - black, which sits badly beside the rest of the palette.
core::const_surface_ptr globe_composite_water(const core::const_surface_ptr& map)
{
	if (!is_valid(map)) return map;

	const auto dims = map->dimensions();
	const auto result = std::make_shared<core::surface>();

	// A failed alloc still reports the requested extent, so the composite is abandoned rather than
	// written through the null it handed back. The uncomposited map draws, over a black ocean.
	if (!result->alloc(dims, core::texture_format::ARGB)) return map;

	// Land is one flat tone, so its colour is whatever the first opaque pixel carries - and a
	// fully opaque pixel reads the same whether the decode premultiplied alpha or not.
	auto land = core::bgr(core::rgb(128, 128, 128));
	auto found_land = false;

	for (auto y = 0; y < dims.cy && !found_land; ++y)
	{
		const auto* const line = std::bit_cast<const core::color32*>(map->pixels_line(y));

		for (auto x = 0; x < dims.cx; ++x)
		{
			if (core::get_a(line[x]) == 255)
			{
				land = line[x] & 0x00FFFFFFu;
				found_land = true;
				break;
			}
		}
	}

	const auto water = core::bgr(core::rgb(19, 38, 64));

	for (auto y = 0; y < dims.cy; ++y)
	{
		const auto* const source = std::bit_cast<const core::color32*>(map->pixels_line(y));
		auto* const destination = std::bit_cast<core::color32*>(result->pixels_line(y));

		for (auto x = 0; x < dims.cx; ++x)
		{
			destination[x] = (core::lerp(water, land, static_cast<int>(core::get_a(source[x]))) & 0x00FFFFFFu) |
			                 0xFF000000u;
		}
	}

	return result;
}

void globe_renderer::set_source(const core::const_surface_ptr& source)
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
		core::surface_ptr reduced;
		if (!core::area_downscale(_levels.back(), reduced, next)) break;
		_levels.emplace_back(std::move(reduced));
		extent = next;
	}
}

bool globe_renderer::render(core::surface& destination, const globe_projection& projection,
                            globe_tile_source* const tiles) const
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
	const auto max_tile_level = tiles ? std::min(tiles->max_level(), globe_max_tile_level) : -1;
	const auto draws_tiles = max_tile_level >= 0;

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
		if (draws_tiles) out.my = mercator_y(lat);
		return true;
	};

	const auto draw_row = [&](const int y, tile_reader& reader)
	{
		const auto uy = (center.y - y) / radius;
		const auto row_half = 1.0 - uy * uy;
		if (row_half <= 0.0) return;

		const auto half_width = std::sqrt(row_half);
		const auto x_first = std::max(0, static_cast<int>(std::ceil(center.x - half_width * radius)));
		const auto x_last = std::min(dims.cx - 1, static_cast<int>(std::floor(center.x + half_width * radius)));
		if (x_first > x_last) return;

		auto* const line = std::bit_cast<core::color32*>(destination.pixels_line(y));
		const auto uy2 = uy * uy;
		auto tile_level = max_tile_level;

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

			const auto exact = span == 0 || std::abs(lat_begin) > globe_pole_guard_rad ||
			                   std::abs(lat_end) > globe_pole_guard_rad;

			// How much of the map one screen pixel covers here chooses the reduced copy to read.
			auto level_index = 0;

			if (span > 0)
			{
				const auto texels_per_pixel = std::max(std::abs(end.u - begin.u), std::abs(end.v - begin.v)) / span;
				if (texels_per_pixel > 1.0)
				{
					level_index = std::clamp(static_cast<int>(std::log2(texels_per_pixel)), 0, max_level);
				}

				// And how much of the world it covers chooses the tile level: the one whose texels are
				// about a pixel each here, finer toward the centre and coarser toward the limb.
				if (draws_tiles)
				{
					const auto world_per_pixel = std::max(std::abs(end.u - begin.u) / source_cx,
					                                      std::abs(end.my - begin.my)) /
					                             span;
					tile_level = world_per_pixel > 0.0
					                 ? std::clamp(static_cast<int>(std::lround(-std::log2(world_per_pixel * 256.0))), 0,
					                              max_tile_level)
					                 : max_tile_level;
				}
			}

			const auto& level = *_levels[level_index];
			const source_level texels{level.pixels(), static_cast<size_t>(level.stride()), level.dimensions().cx,
			                          level.dimensions().cy};
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
					at.my = begin.my + (end.my - begin.my) * t;
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
					const auto covered = core::round(coverage * 255.0);
					if (covered <= 0) continue;

					alpha = static_cast<uint32_t>(std::min(covered, 255));

					// Premultiplied at the limb, matching what the chart rasterisers do where their
					// silhouettes fade to nothing.
					shade_q8 = shade_q8 * alpha / 255u;
				}

				const auto tile = draws_tiles ? reader.sample(tile_level, at.u / source_cx, at.my)
				                              : std::optional<core::color32>{};
				const auto color = tile ? *tile : sample_bilinear(texels, at.u * level_scale, at.v * level_scale_y);
				line[px] = modulate(color, shade_q8, alpha);
			}

			x = x_end + 1;
		}
	};

	// Each pixel depends only on the view, the map and the tiles, so bands of rows are drawn side by side: a
	// globe that fills a large window is drawn in a fraction of the time one thread takes, which is what keeps a
	// drag, a zoom or a turn smooth. Each band reads tiles through its own reader.
	constexpr int band_rows = 16;
	std::vector<std::pair<int, tile_reader>> bands;
	bands.reserve(static_cast<size_t>((dims.cy + band_rows - 1) / band_rows));
	for (auto y = 0; y < dims.cy; y += band_rows) bands.emplace_back(y, tile_reader(tiles));

	const auto row_bytes = static_cast<size_t>(destination.stride());
	const auto draw_band = [&](std::pair<int, tile_reader>& band)
	{
		const auto last = std::min(band.first + band_rows, dims.cy);

		for (auto y = band.first; y < last; ++y)
		{
			memset(destination.pixels_line(y), 0, row_bytes);
			if (y >= y_first && y <= y_last) draw_row(y, band.second);
		}
	};

	// Side by side where the standard library does it on the system's own threads, as MSVC's does on the thread pool.
	// libstdc++ hands parallel algorithms to TBB where its headers are installed, which would then have to be linked,
	// so elsewhere the bands are drawn in turn.
#ifdef _MSC_VER
	if (bands.size() > 1) std::for_each(std::execution::par, bands.begin(), bands.end(), draw_band);
	else std::ranges::for_each(bands, draw_band);
#else
	std::ranges::for_each(bands, draw_band);
#endif

	// The source hears what was wanted on this thread, in the order the rows were drawn.
	if (draws_tiles)
	{
		for (const auto& band : bands)
		{
			for (const auto& t : band.second.wanted()) tiles->want(t.z, t.x, t.y);
		}
	}

	return true;
}
