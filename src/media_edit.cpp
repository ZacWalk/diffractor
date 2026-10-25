// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: A photo's edits applied to its pixels: perspective, straightening, crop and scale -
// resampled with Catmull-Rom, as the edit preview draws - and then the colour adjustments.

#include "pch.h"
#include "files.h"

static simple_transform to_simple_transform(const image_edits& pe)
{
	const auto a = pe.rotation_angle();

	if (df::equiv(a, -90) || df::equiv(a, 270))
	{
		return simple_transform::rot_270;
	}

	if (df::equiv(a, 90) || df::equiv(a, -270))
	{
		return simple_transform::rot_90;
	}

	if (df::equiv(a, 180) || df::equiv(a, -180))
	{
		return simple_transform::rot_180;
	}

	return simple_transform::none;
}

// get_pixel returns transparent black outside the surface, so an interpolated edge sample would drag
// every rotated, straightened or perspective-corrected border toward black. Clamp to the edge instead.
static __forceinline ui::color32 sample_clamped(const ui::surface& src, const int x, const int y)
{
	return src.get_pixel(std::clamp(x, 0, static_cast<int>(src.width()) - 1),
	                     std::clamp(y, 0, static_cast<int>(src.height()) - 1));
}

static __forceinline double catmull_rom_weight(const double t)
{
	const auto a = std::abs(t);
	if (a < 1.0) return ((1.5 * a - 2.5) * a) * a + 1.0;
	if (a < 2.0) return ((-0.5 * a + 2.5) * a - 4.0) * a + 2.0;
	return 0.0;
}

// Catmull-Rom, the same sampler the edit preview draws with, so a saved straighten keeps the detail
// the preview showed rather than the extra softening bilinear leaves behind.
static ui::color32 sample_catmull_rom(const ui::surface& src, const double x, const double y, const bool has_alpha)
{
	const auto left_f = floor(x);
	const auto top_f = floor(y);
	const auto left = static_cast<int>(left_f);
	const auto top = static_cast<int>(top_f);
	const auto offset_x = x - left_f;
	const auto offset_y = y - top_f;

	double weights_x[4];
	double weights_y[4];

	for (auto i = 0; i < 4; ++i)
	{
		weights_x[i] = catmull_rom_weight(offset_x - (i - 1));
		weights_y[i] = catmull_rom_weight(offset_y - (i - 1));
	}

	auto red = 0.0;
	auto green = 0.0;
	auto blue = 0.0;
	auto alpha = 0.0;

	for (auto j = 0; j < 4; ++j)
	{
		for (auto i = 0; i < 4; ++i)
		{
			const auto c = sample_clamped(src, left + i - 1, top + j - 1);
			const auto weight = weights_x[i] * weights_y[j];
			// Filtering straight alpha would let a transparent pixel pull its colour into the result.
			const auto coverage = has_alpha ? weight * (ui::get_a(c) / 255.0) : weight;

			red += ui::get_r(c) * coverage;
			green += ui::get_g(c) * coverage;
			blue += ui::get_b(c) * coverage;
			alpha += coverage;
		}
	}

	if (alpha > 1e-6)
	{
		red /= alpha;
		green /= alpha;
		blue /= alpha;
	}

	return ui::saturate_rgba(df::round(red), df::round(green), df::round(blue),
	                         has_alpha ? df::round(alpha * 255.0) : 255);
}

ui::const_surface_ptr apply_image_edits(const ui::const_surface_ptr& source, const image_edits& photo_edits,
                                        const df::cancel_token& token)
{
	const auto dimensions = source->dimensions();
	const auto format = source->format();

	if (photo_edits.has_perspective())
	{
		auto canvas = std::make_shared<ui::surface>();

		if (!canvas->alloc(dimensions, format)) return {};

		const auto display_angle = to_radian(-photo_edits.crop_bounds(dimensions).angle());
		const auto angle_sin = sin(display_angle);
		const auto angle_cos = cos(display_angle);
		const auto horizontal = photo_edits.perspective_horizontal() * angle_cos -
			photo_edits.perspective_vertical() * angle_sin;
		const auto vertical = photo_edits.perspective_horizontal() * angle_sin +
			photo_edits.perspective_vertical() * angle_cos;
		const auto has_alpha = format == ui::texture_format::ARGB;

		for (auto y = 0u; y < canvas->height(); ++y)
		{
			if (token.is_cancelled()) return {};
			auto* const dst_line = std::bit_cast<ui::color32*>(canvas->pixels_line(y));
			const auto normalized_y = (y + 0.5) / canvas->height() - 0.5;

			for (auto x = 0u; x < canvas->width(); ++x)
			{
				const auto normalized_x = (x + 0.5) / canvas->width() - 0.5;
				const auto denominator = 1.0 + horizontal * normalized_x + vertical * normalized_y;
				// A vanishing denominator produces infinite or NaN source coordinates, and the sampling
				// below indexes with them.
				if (std::abs(denominator) < 1e-12) continue;
				const auto source_x = (0.5 + normalized_x / denominator) * dimensions.cx - 0.5;
				const auto source_y = (0.5 + normalized_y / denominator) * dimensions.cy - 0.5;

				dst_line[x] = sample_catmull_rom(*source, source_x, source_y, has_alpha);
			}
		}

		auto remaining_edits = photo_edits;
		remaining_edits.perspective_horizontal(0);
		remaining_edits.perspective_vertical(0);
		if (photo_edits.has_crop_bounds())
		{
			remaining_edits.crop_bounds(photo_edits.effective_crop_bounds(dimensions));
		}
		return apply_image_edits(canvas, remaining_edits, token);
	}

	ui::const_surface_ptr surface_result;

	const auto rot_angle = photo_edits.rotation_angle();
	const auto is_flip_rotate = !df::is_zero(rot_angle) &&
		df::is_zero(fabs(fmod(rot_angle, 90))) &&
		!photo_edits.has_crop(dimensions) &&
		!photo_edits.has_scale();

	if (is_flip_rotate)
	{
		surface_result = source->transform(to_simple_transform(photo_edits));
	}
	else if (!photo_edits.has_rotation() && !photo_edits.has_scale() && photo_edits.has_crop(dimensions))
	{
		// A crop must not resample. The rectangle arrives from device coordinates and lands on
		// fractional source pixels, so interpolating would shift and soften every pixel it keeps.
		const auto crop = photo_edits.crop_bounds(dimensions).crop(rectd(0, 0, dimensions.cx, dimensions.cy)).
		                              bounding_rect();
		const auto extent = crop.extent().round();
		const auto canvas_extent = sizei(std::clamp(extent.cx, 1, dimensions.cx),
		                                 std::clamp(extent.cy, 1, dimensions.cy));
		const auto left = std::clamp(df::round(crop.left()), 0, dimensions.cx - canvas_extent.cx);
		const auto top = std::clamp(df::round(crop.top()), 0, dimensions.cy - canvas_extent.cy);

		auto canvas = std::make_shared<ui::surface>();

		if (canvas->alloc(canvas_extent, format))
		{
			for (auto y = 0; y < canvas_extent.cy; ++y)
			{
				memcpy(canvas->pixels_line(y), source->pixels() + (top + y) * source->stride() + left * 4,
				       static_cast<size_t>(canvas_extent.cx) * 4);
			}

			surface_result = std::move(canvas);
		}
	}
	else if (photo_edits.has_crop(dimensions) || photo_edits.has_scale() || photo_edits.has_rotation())
	{
		const auto crop = photo_edits.crop_bounds(dimensions).crop(rectd(0, 0, dimensions.cx, dimensions.cy));
		const auto angle = -crop.angle();
		const quadd bounds(dimensions);
		auto aff = affined().translate(-bounds.center_point()).rotate(angle);

		if (photo_edits.has_scale())
		{
			const auto result_dimensions = crop.transform(aff).bounding_rect().extent();
			const auto limit = photo_edits.scale();
			const auto scale = std::min(limit.cx / result_dimensions.Width, limit.cy / result_dimensions.Height);
			aff = aff.scale(scale);
		}

		const auto transformed_crop = crop.transform(aff).bounding_rect();
		aff = aff.translate(-transformed_crop.top_left());

		const auto inc_aff = aff.invert();
		const auto canvas_extent = transformed_crop.extent().round();

		auto canvas = std::make_shared<ui::surface>();

		if (canvas->alloc(canvas_extent, format))
		{
			const auto has_alpha = format == ui::texture_format::ARGB;

			const auto dst_to_src_points0 = inc_aff.transform({0.0, 0.0});
			const auto dst_to_src_points1 = inc_aff.transform({1.0, 0.0});
			const auto dst_to_src_points2 = inc_aff.transform({0.0, 1.0});

			const auto x_dx = dst_to_src_points1.X - dst_to_src_points0.X;
			const auto x_dy = dst_to_src_points1.Y - dst_to_src_points0.Y;
			const auto y_dx = dst_to_src_points2.X - dst_to_src_points0.X;
			const auto y_dy = dst_to_src_points2.Y - dst_to_src_points0.Y;

			for (auto y = 0u; y < canvas->height(); y++)
			{
				auto* const dst_line = std::bit_cast<ui::color32*>(canvas->pixels_line(y));

				for (auto x = 0u; x < canvas->width(); x++)
				{
					const pointd src_pointf = {
						dst_to_src_points0.X + x * x_dx + y * y_dx,
						dst_to_src_points0.Y + x * x_dy + y * y_dy
					};

					const auto leftxf = floor(src_pointf.X);
					const auto leftx = static_cast<int>(leftxf);
					const auto rightx = static_cast<int>(ceil(src_pointf.X));
					const auto topyf = floor(src_pointf.Y);
					const auto topy = static_cast<int>(topyf);
					const auto bottomy = static_cast<int>(ceil(src_pointf.Y));

					if (leftx == rightx && topy == bottomy)
					{
						dst_line[x] = source->get_pixel(leftx, topy);
					}
					else
					{
						dst_line[x] = sample_catmull_rom(*source, src_pointf.X, src_pointf.Y, has_alpha);
					}
				}
			}

			surface_result = std::move(canvas);
		}
	}
	else
	{
		surface_result = source;
	}

	if (photo_edits.has_color_changes())
	{
		// A geometry step that could not allocate leaves nothing to adjust. Answering the empty
		// surface here is the failure; reading its dimensions would be a null dereference.
		if (!surface_result) return {};

		ui::color_adjust adjust;
		adjust.color_params(photo_edits.vibrance(), photo_edits.saturation(), photo_edits.darks(),
		                    photo_edits.midtones(),
		                    photo_edits.lights(), photo_edits.contrast(), photo_edits.brightness(),
		                    photo_edits.temperature(), photo_edits.tint());

		auto canvas = std::make_shared<ui::surface>();

		if (canvas->alloc(surface_result->dimensions(), surface_result->format()))
		{
			// A cancelled pass leaves the rows it never reached uninitialised, so the canvas is
			// dropped rather than published: the caller asked for this result and no longer wants it.
			if (!adjust.apply(surface_result, canvas->pixels(), canvas->stride(), token)) return {};
			surface_result = std::move(canvas);
		}
		else
		{
			return {};
		}
	}

	return surface_result;
}
