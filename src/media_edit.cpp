// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: A photo's edits applied to its pixels, a byte a channel or sixteen bits: perspective,
// straightening, crop and scale - resampled with Catmull-Rom, as the edit preview draws - and then the
// colour adjustments, which an HDR picture's signal takes as its SDR rendition is shown taking them, and
// colours an ICC profile describes as their sRGB rendition is.

#include "pch.h"
#include "files.h"
#include "libfiles/hdr.h"
#include "libfiles/icc.h"

static simple_transform to_simple_transform(const image_edits& pe)
{
	const auto a = pe.rotation_angle();

	if (core::equiv(a, -90) || core::equiv(a, 270))
	{
		return core::simple_transform::rot_270;
	}

	if (core::equiv(a, 90) || core::equiv(a, -270))
	{
		return core::simple_transform::rot_90;
	}

	if (core::equiv(a, 180) || core::equiv(a, -180))
	{
		return core::simple_transform::rot_180;
	}

	return core::simple_transform::none;
}

// Pixels are four channels, a byte each or sixteen bits each, the fourth alpha where the format has it:
// what follows treats the three colour channels alike, so their order does not matter to it.

// get_pixel answers transparent black outside the surface, so an interpolated edge sample would drag
// every rotated, straightened or perspective-corrected border toward black. Clamp to the edge instead.
template <typename T> static __forceinline const T* clamped_pixel(const core::surface& src, const int x, const int y)
{
	const auto cx = std::clamp(x, 0, static_cast<int>(src.width()) - 1);
	const auto cy = std::clamp(y, 0, static_cast<int>(src.height()) - 1);
	return std::bit_cast<const T*>(src.pixels_line(cy)) + static_cast<size_t>(cx) * 4;
}

// A pixel where it lies, or transparent black outside the surface, as get_pixel answers.
template <typename T>
static __forceinline void copy_pixel(const core::surface& src, const int x, const int y, T* const out)
{
	if (x < 0 || y < 0 || x >= static_cast<int>(src.width()) || y >= static_cast<int>(src.height()))
	{
		out[0] = out[1] = out[2] = out[3] = 0;
		return;
	}

	std::memcpy(out, std::bit_cast<const T*>(src.pixels_line(y)) + static_cast<size_t>(x) * 4, 4 * sizeof(T));
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
template <typename T>
static void sample_catmull_rom(const core::surface& src, const double x, const double y, const bool has_alpha,
                               T* const out)
{
	constexpr auto maximum = static_cast<double>(std::numeric_limits<T>::max());
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

	double colour[3] = {};
	auto alpha = 0.0;

	for (auto j = 0; j < 4; ++j)
	{
		for (auto i = 0; i < 4; ++i)
		{
			const auto* const c = clamped_pixel<T>(src, left + i - 1, top + j - 1);
			const auto weight = weights_x[i] * weights_y[j];
			// Filtering straight alpha would let a transparent pixel pull its colour into the result.
			const auto coverage = has_alpha ? weight * (c[3] / maximum) : weight;

			for (auto channel = 0; channel < 3; ++channel) colour[channel] += c[channel] * coverage;
			alpha += coverage;
		}
	}

	if (alpha > 1e-6)
	{
		for (auto& channel : colour) channel /= alpha;
	}

	for (auto channel = 0; channel < 3; ++channel)
	{
		out[channel] = static_cast<T>(std::clamp(std::round(colour[channel]), 0.0, maximum));
	}

	out[3] = static_cast<T>(has_alpha ? std::clamp(std::round(alpha * maximum), 0.0, maximum) : maximum);
}

core::const_surface_ptr apply_image_edits(const core::const_surface_ptr& source, const image_edits& photo_edits,
                                          const core::cancel_token& token, const libfiles::hdr_description* const hdr,
                                          const std::span<const uint8_t> profile)
{
	const auto dimensions = source->dimensions();
	const auto format = source->format();
	// Sixteen bits a channel are resampled at sixteen, so a wide picture keeps its precision to the save.
	const auto wide = core::is_wide(format);
	const auto has_alpha = core::has_alpha(format);

	if (photo_edits.has_perspective())
	{
		auto canvas = std::make_shared<core::surface>();

		if (!canvas->alloc(dimensions, format)) return {};

		const auto display_angle = to_radian(-photo_edits.crop_bounds(dimensions).angle());
		const auto angle_sin = sin(display_angle);
		const auto angle_cos = cos(display_angle);
		const auto horizontal = photo_edits.perspective_horizontal() * angle_cos -
		                        photo_edits.perspective_vertical() * angle_sin;
		const auto vertical = photo_edits.perspective_horizontal() * angle_sin +
		                      photo_edits.perspective_vertical() * angle_cos;

		const auto correct = [&](auto channel)
		{
			using T = decltype(channel);

			for (auto y = 0u; y < canvas->height(); ++y)
			{
				if (token.is_cancelled()) return false;
				auto* const dst_line = std::bit_cast<T*>(canvas->pixels_line(y));
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

					sample_catmull_rom(*source, source_x, source_y, has_alpha, dst_line + static_cast<size_t>(x) * 4);
				}
			}

			return true;
		};

		if (!(wide ? correct(uint16_t{}) : correct(uint8_t{}))) return {};

		auto remaining_edits = photo_edits;
		remaining_edits.perspective_horizontal(0);
		remaining_edits.perspective_vertical(0);
		if (photo_edits.has_crop_bounds())
		{
			remaining_edits.crop_bounds(photo_edits.effective_crop_bounds(dimensions));
		}
		return apply_image_edits(canvas, remaining_edits, token, hdr, profile);
	}

	core::const_surface_ptr surface_result;

	const auto rot_angle = photo_edits.rotation_angle();
	const auto is_flip_rotate = !core::is_zero(rot_angle) && core::is_zero(fabs(fmod(rot_angle, 90))) &&
	                            !photo_edits.has_crop(dimensions) && !photo_edits.has_scale();

	if (is_flip_rotate)
	{
		surface_result = source->transform(to_simple_transform(photo_edits));
	}
	else if (!photo_edits.has_rotation() && !photo_edits.has_scale() && photo_edits.has_crop(dimensions))
	{
		// A crop must not resample. The rectangle arrives from device coordinates and lands on
		// fractional source pixels, so interpolating would shift and soften every pixel it keeps.
		const auto crop =
			photo_edits.crop_bounds(dimensions).crop(rectd(0, 0, dimensions.cx, dimensions.cy)).bounding_rect();
		const auto extent = crop.extent().round();
		const auto canvas_extent = sizei(std::clamp(extent.cx, 1, dimensions.cx),
		                                 std::clamp(extent.cy, 1, dimensions.cy));
		const auto left = std::clamp(core::round(crop.left()), 0, dimensions.cx - canvas_extent.cx);
		const auto top = std::clamp(core::round(crop.top()), 0, dimensions.cy - canvas_extent.cy);

		auto canvas = std::make_shared<core::surface>();

		if (canvas->alloc(canvas_extent, format))
		{
			const auto pixel_bytes = static_cast<size_t>(core::bytes_per_pixel(format));

			for (auto y = 0; y < canvas_extent.cy; ++y)
			{
				memcpy(canvas->pixels_line(y), source->pixels() + (top + y) * source->stride() + left * pixel_bytes,
				       static_cast<size_t>(canvas_extent.cx) * pixel_bytes);
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

		auto canvas = std::make_shared<core::surface>();

		if (canvas->alloc(canvas_extent, format))
		{
			const auto dst_to_src_points0 = inc_aff.transform({0.0, 0.0});
			const auto dst_to_src_points1 = inc_aff.transform({1.0, 0.0});
			const auto dst_to_src_points2 = inc_aff.transform({0.0, 1.0});

			const auto x_dx = dst_to_src_points1.X - dst_to_src_points0.X;
			const auto x_dy = dst_to_src_points1.Y - dst_to_src_points0.Y;
			const auto y_dx = dst_to_src_points2.X - dst_to_src_points0.X;
			const auto y_dy = dst_to_src_points2.Y - dst_to_src_points0.Y;

			const auto resample = [&](auto channel)
			{
				using T = decltype(channel);

				for (auto y = 0u; y < canvas->height(); y++)
				{
					auto* const dst_line = std::bit_cast<T*>(canvas->pixels_line(y));

					for (auto x = 0u; x < canvas->width(); x++)
					{
						const pointd src_pointf = {dst_to_src_points0.X + x * x_dx + y * y_dx,
						                           dst_to_src_points0.Y + x * x_dy + y * y_dy};

						const auto leftxf = floor(src_pointf.X);
						const auto leftx = static_cast<int>(leftxf);
						const auto rightx = static_cast<int>(ceil(src_pointf.X));
						const auto topyf = floor(src_pointf.Y);
						const auto topy = static_cast<int>(topyf);
						const auto bottomy = static_cast<int>(ceil(src_pointf.Y));
						auto* const dst = dst_line + static_cast<size_t>(x) * 4;

						if (leftx == rightx && topy == bottomy)
						{
							copy_pixel(*source, leftx, topy, dst);
						}
						else
						{
							sample_catmull_rom(*source, src_pointf.X, src_pointf.Y, has_alpha, dst);
						}
					}
				}
			};

			if (wide) resample(uint16_t{});
			else resample(uint8_t{});

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

		render::color_adjust adjust;
		adjust.color_params(photo_edits.vibrance(), photo_edits.saturation(), photo_edits.darks(),
		                    photo_edits.midtones(), photo_edits.lights(), photo_edits.contrast(),
		                    photo_edits.brightness(), photo_edits.temperature(), photo_edits.tint());

		if (hdr && core::is_wide(surface_result->format()))
		{
			// An HDR picture's colour is changed as its SDR rendition - what the edit shows - is: the rendition is
			// adjusted, and the signal's light scaled to match, so what lies above SDR white keeps its headroom over
			// the picture beneath it.
			const auto before = sdr_rendition(surface_result, *hdr, true);
			auto after = std::make_shared<core::surface>();
			auto signal = std::make_shared<core::surface>();
			if (!before || !after->alloc(before->dimensions(), before->format()) ||
			    !signal->alloc(surface_result->dimensions(), surface_result->format()))
			{
				return {};
			}

			if (!adjust.apply(before, after->pixels(), after->stride(), token)) return {};

			const auto view = [](const core::surface& s)
			{
				return libfiles::image_view{s.pixels(), s.stride(), s.width(), s.height(),
				                            core::has_alpha(s.format()) ? libfiles::pixel_format::rgba16
				                                                        : libfiles::pixel_format::rgbx16};
			};

			if (!libfiles::carry_sdr_edit(view(*surface_result), *hdr, view(*before), view(*after),
			                              {signal->pixels(), signal->stride()}))
			{
				return {};
			}

			return signal;
		}

		if (!hdr && !profile.empty())
		{
			// Colours a profile describes are changed as their sRGB rendition - what the edit shows - is: the
			// rendition is adjusted, and the change carried back into the profile's colours, which the profile kept
			// with them still describes, and which keep what of its gamut lies past sRGB's where the edit left it.
			const auto before = profile_rendition(surface_result, profile, true);
			if (!before) return {};

			if (before != surface_result)
			{
				auto after = std::make_shared<core::surface>();
				auto carried = std::make_shared<core::surface>();
				if (!after->alloc(before->dimensions(), before->format()) ||
				    !carried->alloc(surface_result->dimensions(), surface_result->format()))
				{
					return {};
				}

				if (!adjust.apply(before, after->pixels(), after->stride(), token)) return {};

				const auto pixels = libfiles_view(*surface_result);
				const auto shown = libfiles_view(*before);
				const auto edited = libfiles_view(*after);
				if (!pixels || !shown || !edited ||
				    !libfiles::carry_srgb_edit(*pixels, profile, *shown, *edited,
				                               {carried->pixels(), carried->stride()}))
				{
					return {};
				}

				return carried;
			}
		}

		auto canvas = std::make_shared<core::surface>();

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
