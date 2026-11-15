// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The rules face detection's input and output obey - the luminance plane the detector reads,
// that plane turned upright, and the stable order faces are reported in. Owned by docs/faces.md. No
// file, database or window is touched here.

#include "pch.h"

#include "model_faces.h"

namespace faces
{
	const tuning& defaults()
	{
		static const tuning t;
		return t;
	}

	// The detector reads only luma, including odd output extents that an NV12 surface cannot hold.
	// Match the shared scaler's Q8 weights and rounding without processing or allocating chroma.
	static bool reduce_luma_plane(const core::const_surface_ptr& surface, const sizei destination,
	                              std::vector<uint8_t>& result)
	{
		const auto source = surface->dimensions();
		if (destination.cx > source.cx || destination.cy > source.cy) return false;

		struct span
		{
			int first;
			std::vector<uint16_t> weights;
		};
		const auto axis = [](const int source_count, const int destination_count)
		{
			std::vector<span> spans;
			spans.reserve(destination_count);
			const auto scale = static_cast<double>(source_count) / destination_count;
			for (auto i = 0; i < destination_count; ++i)
			{
				const auto start = i * scale;
				const auto end = (i + 1) * scale;
				const auto first = std::clamp(static_cast<int>(start), 0, source_count - 1);
				const auto last = std::clamp(static_cast<int>(std::ceil(end)) - 1, first, source_count - 1);
				span sample{first, {}};
				auto previous = 0;
				for (auto j = first; j <= last; ++j)
				{
					const auto edge = std::min(end, static_cast<double>(j + 1));
					const auto covered = std::clamp((edge - start) / (end - start), 0.0, 1.0);
					const auto cumulative = j == last ? 256
					                                  : std::clamp(static_cast<int>(std::lround(covered * 256.0)),
					                                               previous, 256);
					sample.weights.emplace_back(static_cast<uint16_t>(cumulative - previous));
					previous = cumulative;
				}
				spans.emplace_back(std::move(sample));
			}
			return spans;
		};
		const auto horizontal = axis(source.cx, destination.cx);
		const auto vertical = axis(source.cy, destination.cy);
		std::vector<uint16_t> accumulated(source.cx);
		std::vector<uint8_t> reduced(source.cx);
		for (auto y = 0; y < destination.cy; ++y)
		{
			std::ranges::fill(accumulated, uint16_t{0});
			const auto& rows = vertical[y];
			for (auto k = 0_z; k < rows.weights.size(); ++k)
			{
				const auto* pixels = surface->pixels() + (rows.first + k) * surface->stride();
				for (auto x = 0; x < source.cx; ++x)
					accumulated[x] = static_cast<uint16_t>(accumulated[x] + pixels[x] * rows.weights[k]);
			}
			for (auto x = 0; x < source.cx; ++x) reduced[x] = static_cast<uint8_t>((accumulated[x] + 128) >> 8);
			auto* output = result.data() + static_cast<size_t>(y) * destination.cx;
			for (auto x = 0; x < destination.cx; ++x)
			{
				const auto& columns = horizontal[x];
				uint32_t accumulated_pixel = 0;
				for (auto k = 0_z; k < columns.weights.size(); ++k)
					accumulated_pixel += reduced[columns.first + k] * columns.weights[k];
				output[x] = static_cast<uint8_t>((accumulated_pixel + 128) >> 8);
			}
		}
		return true;
	}

	bool reduce_to_luma(const core::const_surface_ptr& surface, const sizei destination, core::surface_ptr& scratch,
	                    std::vector<uint8_t>& result)
	{
		if (!core::is_valid(surface) || destination.cx <= 0 || destination.cy <= 0) return false;
		const auto packed = core::is_packed(surface->format());
		if (!packed && surface->format() != core::texture_format::NV12) return false;
		result.resize(static_cast<size_t>(destination.cx) * destination.cy);
		if (packed)
		{
			auto source = surface;
			if (source->dimensions() != destination)
			{
				if (!core::area_downscale(source, scratch, destination)) return false;
				source = scratch;
			}
			for (auto row = 0; row < destination.cy; ++row)
			{
				const auto* pixels = source->pixels_line(row);
				auto* output = result.data() + static_cast<size_t>(row) * destination.cx;
				for (auto column = 0; column < destination.cx; ++column)
				{
					const auto* pixel = pixels + static_cast<size_t>(column) * 4;
					output[column] = static_cast<uint8_t>((pixel[0] * 29 + pixel[1] * 150 + pixel[2] * 77 + 128) >> 8);
				}
			}
			return true;
		}
		if (!reduce_luma_plane(surface, destination, result)) return false;
		const auto color_space = surface->color_space();
		if (color_space == core::color_space::rec601_limited || color_space == core::color_space::rec709_limited ||
		    color_space == core::color_space::rec2020_limited)
		{
			for (auto& sample : result)
				sample = static_cast<uint8_t>(std::clamp(((static_cast<int>(sample) - 16) * 255 + 109) / 219, 0, 255));
		}
		return true;
	}

	sizei upright_extent(const sizei extent, const core::orientation orientation)
	{
		return core::flips_xy(orientation) ? sizei{extent.cy, extent.cx} : extent;
	}

	bool orient_plane(const std::span<const uint8_t> plane, const sizei extent, const core::orientation orientation,
	                  std::vector<uint8_t>& result)
	{
		const auto w = extent.cx;
		const auto h = extent.cy;
		if (w <= 0 || h <= 0 || plane.size() < static_cast<size_t>(w) * h) return false;

		// Where stored pixel (x, y) is shown: upright x = ax*x + bx*y + cx, upright y = ay*x + by*y + cy.
		// Each case is the EXIF meaning of the orientation - right_top, for one, is a picture whose
		// stored first row is its right-hand side, so its stored top-left corner is shown top right.
		struct mapping
		{
			int ax, bx, cx;
			int ay, by, cy;
		};

		const auto m = [&]() -> mapping
		{
			switch (orientation)
			{
			case core::orientation::top_right: return {-1, 0, w - 1, 0, 1, 0};
			case core::orientation::bottom_right: return {-1, 0, w - 1, 0, -1, h - 1};
			case core::orientation::bottom_left: return {1, 0, 0, 0, -1, h - 1};
			case core::orientation::left_top: return {0, 1, 0, 1, 0, 0};
			case core::orientation::right_top: return {0, -1, h - 1, 1, 0, 0};
			case core::orientation::right_bottom: return {0, -1, h - 1, -1, 0, w - 1};
			case core::orientation::left_bottom: return {0, 1, 0, -1, 0, w - 1};
			default: return {1, 0, 0, 0, 1, 0};
			}
		}();

		const auto upright_width = upright_extent(extent, orientation).cx;
		result.resize(static_cast<size_t>(w) * h);

		for (auto y = 0; y < h; ++y)
		{
			const auto* const row = plane.data() + static_cast<size_t>(y) * w;

			for (auto x = 0; x < w; ++x)
			{
				const auto ux = m.ax * x + m.bx * y + m.cx;
				const auto uy = m.ay * x + m.by * y + m.cy;
				result[static_cast<size_t>(uy) * upright_width + ux] = row[x];
			}
		}

		return true;
	}

	void order_faces(std::vector<face>& faces)
	{
		std::ranges::sort(faces, [](const face& a, const face& b)
		{
			// Rounded to whole pixels first so that two boxes differing by a fraction do not swap when
			// the same picture is read again at a different bound.
			const auto ax = core::round(a.bounds.X);
			const auto bx = core::round(b.bounds.X);
			if (ax != bx) return ax < bx;

			const auto ay = core::round(a.bounds.Y);
			const auto by = core::round(b.bounds.Y);
			if (ay != by) return ay < by;

			const auto aw = core::round(a.bounds.Width);
			const auto bw = core::round(b.bounds.Width);
			if (aw != bw) return aw > bw;

			const auto ah = core::round(a.bounds.Height);
			const auto bh = core::round(b.bounds.Height);
			if (ah != bh) return ah > bh;

			return a.confidence > b.confidence;
		});
	}

	face_set find_faces(face_detector& detector, const core::const_surface_ptr& surface, const tuning& t)
	{
		face_set result;
		if (!core::is_valid(surface)) return result;

		result.extent = upright_extent(surface->dimensions(), surface->orientation());
		result.faces = detector.detect(surface, t);
		order_faces(result.faces);
		return result;
	}
}
