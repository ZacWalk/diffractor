// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tests for the rendering layer (render*, util_simd) -- software blends, YUV conversion for the software backend, packed-surface area downscaling, surface transforms (resize, rotate) and the drawn logo.

#include "pch.h"
#include "test.h"
#include "libdesktop/util_simd.h"
#include "av_format.h"
#include "libdesktop/render_software.h"
#include "test_fixtures.h"
#include "test_runner.h"
#include "ui_elements.h"
#include "ui_charts.h"
#include "ui_globe.h"
#include "ui_panorama.h"

static uint8_t blend_opaque_channel(const uint8_t dest, const float src, const float alpha)
{
	return static_cast<uint8_t>(std::clamp(static_cast<int>(src * alpha + dest * (1.0f - alpha) + 0.5f), 0, 255));
}

static void should_unpremultiply_surface_alpha()
{
	ui::surface surface;
	assert_equal(true, surface.alloc(3, 1, ui::texture_format::ARGB) != nullptr, "alpha fixture allocated");
	surface.set_pixel(0, 0, 0x80402010u);
	surface.set_pixel(1, 0, 0x00112233u);
	surface.set_pixel(2, 0, 0xff336699u);
	surface.unpremultiply_alpha();
	assert_equal(0x80804020u, surface.get_pixel(0, 0), "associated channels are divided by alpha");
	assert_equal(0u, surface.get_pixel(1, 0), "transparent pixels have defined zero colors");
	assert_equal(0xff336699u, surface.get_pixel(2, 0), "opaque pixels are unchanged");
}

static uint8_t blend_opaque_normalized_channel(const uint8_t dest, const float src, const float alpha)
{
	const auto value = src * alpha + (dest / 255.0f) * (1.0f - alpha);
	return static_cast<uint8_t>(std::clamp(static_cast<int>(value * 255.0f + 0.5f), 0, 255));
}

static void should_match_simd_software_blends()
{
#if defined(COMPILE_SIMD_INTRINSIC)
	constexpr size_t max_pixels = 19;
	alignas(16) std::array<uint8_t, max_pixels * 4> original{};
	alignas(16) std::array<uint8_t, max_pixels * 4> source{};
	alignas(16) std::array<uint8_t, max_pixels> coverage{};

	for (size_t i = 0; i < original.size(); ++i) original[i] = static_cast<uint8_t>(i * 47u + 13u);
	for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint8_t>(i * 29u + 7u);
	for (size_t i = 0; i < coverage.size(); ++i) coverage[i] = static_cast<uint8_t>(i * 61u + 3u);

	constexpr std::array<float, 5> alphas = {0.0f, 0.13f, 0.5f, 0.87f, 1.0f};
	constexpr float sb = 37.0f;
	constexpr float sg = 149.0f;
	constexpr float sr = 231.0f;

	for (size_t count = 0; count <= max_pixels; ++count)
	{
		for (const auto alpha : alphas)
		{
			auto check = [&](auto&& scalar, auto&& simd, const std::string_view message)
			{
				auto expected = original;
				auto actual = original;
				for (size_t i = 0; i < count; ++i) scalar(expected.data() + i * 4, i);
				const auto processed = simd(actual.data());
				assert_equal(count & ~size_t{3}, processed, message);
				for (size_t i = processed; i < count; ++i) scalar(actual.data() + i * 4, i);
				assert_equal(true, expected == actual, message);
			};

			auto blend_solid_pixel = [](uint8_t* dest, const float b, const float g, const float r, const float a)
			{
				dest[0] = blend_opaque_normalized_channel(dest[0], b, a);
				dest[1] = blend_opaque_normalized_channel(dest[1], g, a);
				dest[2] = blend_opaque_normalized_channel(dest[2], r, a);
				dest[3] = 255;
			};

			check([&](uint8_t* dest, size_t) { blend_solid_pixel(dest, sb / 255.0f, sg / 255.0f, sr / 255.0f, alpha); },
			      [&](uint8_t* dest)
			      {
				      return blend_solid_opaque_sse2(dest, count, sb / 255.0f, sg / 255.0f,
				                                     sr / 255.0f, alpha);
			      }, "SIMD solid blend");

			check([&](uint8_t* dest, const size_t i)
			      {
				      blend_solid_pixel(dest, sb / 255.0f, sg / 255.0f,
				                        sr / 255.0f, alpha * coverage[i] / 255.0f);
			      },
			      [&](uint8_t* dest)
			      {
				      return blend_glyph_opaque_sse2(dest, coverage.data(), count, sb / 255.0f,
				                                     sg / 255.0f, sr / 255.0f, alpha);
			      }, "SIMD glyph blend");

			for (const auto has_alpha : {false, true})
			{
				check([&](uint8_t* dest, const size_t i)
				      {
					      const auto* src = source.data() + i * 4;
					      const auto pixel_alpha = (has_alpha ? src[3] / 255.0f : 1.0f) * alpha;
					      dest[0] = blend_opaque_channel(dest[0], src[0], pixel_alpha);
					      dest[1] = blend_opaque_channel(dest[1], src[1], pixel_alpha);
					      dest[2] = blend_opaque_channel(dest[2], src[2], pixel_alpha);
					      dest[3] = 255;
				      }, [&](uint8_t* dest)
				      {
					      return blend_bgra_opaque_sse2(dest, source.data(), count, has_alpha, alpha);
				      }, "SIMD BGRA blend");
			}
		}
	}
#endif
}

// The CPU renderer and every CPU conversion map HDR through the cube the GPU samples, so the lookup has
// to be the arithmetic of a trilinear texture sample: the signal scaled onto the samples, with 0 and
// 65535 on the end samples, and the eight around it blended by distance. Each output channel is a
// different function of the axes, so a swapped axis, channel or byte order shows; the SIMD path and
// the one other architectures run must both agree with an independent double-precision sample.
static void should_tone_map_through_a_cube_as_a_trilinear_sample()
{
	constexpr int n = 5;
	ui::tone_map_lut lut;
	lut.size = n;
	lut.rgba.resize(static_cast<size_t>(n) * n * n * 4);

	const auto at = [](const int r, const int g, const int b)
	{
		const auto fr = r / (n - 1.0);
		const auto fg = g / (n - 1.0);
		const auto fb = b / (n - 1.0);
		return std::array{fr, fg * fg * (1.0 - 0.5 * fb), fb * (1.0 - 0.5 * fr) + 0.25 * fg * fr};
	};

	for (auto b = 0; b < n; ++b)
	{
		for (auto g = 0; g < n; ++g)
		{
			for (auto r = 0; r < n; ++r)
			{
				const auto v = at(r, g, b);
				auto* const out = lut.rgba.data() + ((static_cast<size_t>(b) * n + g) * n + r) * 4;
				for (auto c = 0; c < 3; ++c) out[c] = static_cast<uint16_t>(std::lround(std::clamp(v[c], 0.0, 1.0) * 65535.0));
				out[3] = 0xffff;
			}
		}
	}

	// The sample a texture unit would take, in the cube's own 16-bit units.
	const auto expected = [&lut](const uint16_t* const px)
	{
		std::array<double, 3> result = {};
		std::array<int, 3> index = {};
		std::array<double, 3> weight = {};

		for (auto axis = 0; axis < 3; ++axis)
		{
			const auto x = px[axis] / 65535.0 * (n - 1);
			index[axis] = std::min(static_cast<int>(x), n - 2);
			weight[axis] = x - index[axis];
		}

		for (auto corner = 0; corner < 8; ++corner)
		{
			auto w = 1.0;
			std::array<int, 3> sample = {};

			for (auto axis = 0; axis < 3; ++axis)
			{
				const auto upper = (corner >> axis & 1) != 0;
				sample[axis] = index[axis] + (upper ? 1 : 0);
				w *= upper ? weight[axis] : 1.0 - weight[axis];
			}

			const auto* const s = lut.rgba.data() + ((static_cast<size_t>(sample[2]) * n + sample[1]) * n + sample[0]) * 4;
			for (auto c = 0; c < 3; ++c) result[c] += w * s[c];
		}

		return result;
	};

	std::vector<uint16_t> signal = {0, 0, 0, 65535, 65535, 65535, 65535, 0, 0, 0, 65535, 0, 0, 0, 65535, 32768, 16384, 49152};
	uint32_t seed = 12345;
	for (auto i = 0; i < 3 * 500; ++i)
	{
		seed = seed * 1664525u + 1013904223u;
		signal.push_back(static_cast<uint16_t>(seed >> 16));
	}

	const auto pixels = signal.size() / 3;
	std::vector<ui::color32> fast(pixels);
	std::vector<ui::color32> baseline(pixels);
	lut.apply(signal.data(), pixels, fast.data());
	lut.apply_baseline(signal.data(), pixels, baseline.data());

	auto worst = 0;

	for (size_t i = 0; i < pixels; ++i)
	{
		const auto want = expected(signal.data() + i * 3);

		for (const auto& [name, got] : {std::pair{"SIMD", fast[i]}, std::pair{"baseline", baseline[i]}})
		{
			// Blue is the low byte of a packed RGB pixel, red the third, and the cube's alpha is opaque.
			const std::array channels = {static_cast<int>(got >> 16 & 0xff), static_cast<int>(got >> 8 & 0xff),
			                             static_cast<int>(got & 0xff)};
			assert_equal(0xffu, got >> 24, std::format("{} pixel {} is opaque", name, i));

			for (auto c = 0; c < 3; ++c)
			{
				worst = std::max(worst, std::abs(channels[c] - static_cast<int>(std::lround(want[c] * 255.0 / 65535.0))));
			}
		}
	}

	assert_equal(true, worst <= 1, std::format("both lookups take the trilinear sample to within one level ({})", worst));

	const auto sample_pixel = [&lut](const int r, const int g, const int b)
	{
		const auto* const s = lut.rgba.data() + ((static_cast<size_t>(b) * n + g) * n + r) * 4;
		const auto byte = [](const uint16_t v) { return static_cast<uint32_t>(std::lround(v * 255.0 / 65535.0)); };
		return 0xff000000u | byte(s[0]) << 16 | byte(s[1]) << 8 | byte(s[2]);
	};

	assert_equal(sample_pixel(n - 1, n - 1, n - 1), fast[1], "the brightest signal takes the last sample");
	assert_equal(sample_pixel(0, 0, 0), fast[0], "and the darkest the first");
	assert_equal(sample_pixel(n - 1, 0, 0), fast[2], "full red takes the sample at the end of the red axis");
}

static void should_convert_yuv_surfaces_for_software_rendering()
{
	auto check_output = [](const ui::surface_ptr& output)
	{
		assert_equal(true, ui::is_valid(output), "converted YUV surface is valid");
		assert_equal(uint32_t{4}, output->width(), "converted YUV width");
		assert_equal(uint32_t{2}, output->height(), "converted YUV height");
		for (auto y = 0; y < 2; ++y)
		{
			const auto* const row = output->pixels_line(y);
			for (auto x = 0; x < 4; ++x)
			{
				const auto* const pixel = row + x * 4;
				const auto expected = x < 2 ? 0 : 255;
				assert_equal(true, std::abs(static_cast<int>(pixel[0]) - expected) <= 2, "YUV blue channel");
				assert_equal(true, std::abs(static_cast<int>(pixel[1]) - expected) <= 2, "YUV green channel");
				assert_equal(true, std::abs(static_cast<int>(pixel[2]) - expected) <= 2, "YUV red channel");
				assert_equal(255, pixel[3], "YUV alpha channel");
			}
		}
	};

	av_scaler scaler;
	const auto output = std::make_shared<ui::surface>();
	ui::surface nv12;
	nv12.alloc(4, 2, ui::texture_format::NV12);
	nv12.color_space(ui::color_space::rec601_limited);
	for (auto y = 0; y < 2; ++y)
	{
		auto* const row = nv12.pixels_line(y);
		row[0] = row[1] = 16;
		row[2] = row[3] = 235;
	}
	auto* const nv12_chroma = nv12.pixels() + nv12.stride() * nv12.height();
	std::fill_n(nv12_chroma, nv12.stride(), uint8_t{128});
	assert_equal(true, scaler.convert_yuv_surface(nv12, output), "convert NV12 surface");
	check_output(output);
	ui::surface_ptr scaled;
	const auto nv12_view = ui::const_surface_ptr(&nv12, [](const ui::surface*)
	{
	});
	assert_equal(true, scaler.scale_surface(nv12_view, scaled, {2, 1}), "scale NV12 surface");
	assert_equal(true, scaled->dimensions() == sizei{2, 1}, "scaled NV12 dimensions");
	assert_equal(true, scaled->format() == ui::texture_format::RGB, "scaled NV12 format");

	ui::surface p010;
	p010.alloc(4, 2, ui::texture_format::P010);
	p010.color_space(ui::color_space::rec601_limited);
	for (auto y = 0; y < 2; ++y)
	{
		auto* const row = std::bit_cast<uint16_t*>(p010.pixels_line(y));
		row[0] = row[1] = 64u << 6;
		row[2] = row[3] = 940u << 6;
	}
	auto* const p010_chroma = std::bit_cast<uint16_t*>(p010.pixels() + p010.stride() * p010.height());
	std::fill_n(p010_chroma, p010.stride() / sizeof(uint16_t), uint16_t{512u << 6});
	assert_equal(true, scaler.convert_yuv_surface(p010, output), "convert P010 surface");
	check_output(output);
	const auto p010_view = ui::const_surface_ptr(&p010, [](const ui::surface*)
	{
	});
	assert_equal(true, scaler.scale_surface(p010_view, scaled, {2, 1}), "scale P010 surface");
	assert_equal(true, scaled->dimensions() == sizei{2, 1}, "scaled P010 dimensions");
	assert_equal(true, scaled->format() == ui::texture_format::RGB, "scaled P010 format");
}

// An enlargement is what area_downscale refuses, and it is the case the software backend hits every
// frame when the window is larger than the video. swscale answers it with an RGB->YUV->RGB round
// trip through its scalar full-chroma converter, so this path exists to keep it out of that.
static void should_bilinear_resize_packed_surfaces()
{
	const auto make_source = [](const int cx, const int cy)
	{
		auto s = std::make_shared<ui::surface>();
		s->alloc(cx, cy, ui::texture_format::ARGB, ui::orientation::left_bottom);
		s->color_space(ui::color_space::rec709_limited);
		return s;
	};

	// Two samples widened to four. Sampling at centres and clamping the outer half sample is what
	// keeps the ends exact; extrapolating instead is what puts a dark fringe round an enlargement.
	{
		const auto src = make_source(2, 1);
		auto* const row = src->pixels_line(0);
		row[0] = row[1] = row[2] = row[3] = 0;
		row[4] = row[5] = row[6] = row[7] = 200;

		ui::surface_ptr dst;
		assert_equal(true, ui::bilinear_resize(src, dst, {4, 1}), "widen a two sample ramp");
		assert_equal(true, dst->dimensions() == sizei{4, 1}, "enlarged dimensions");
		assert_equal(true, dst->format() == ui::texture_format::ARGB, "enlarged format");
		assert_equal(true, dst->orientation() == ui::orientation::left_bottom, "enlarged orientation");
		assert_equal(true, dst->color_space() == ui::color_space::rec709_limited, "enlarged colour space");

		const auto* const out = dst->pixels_line(0);
		assert_equal(0, static_cast<int>(out[0]), "first sample keeps the source end");
		assert_equal(50, static_cast<int>(out[4]), "quarter weight");
		assert_equal(150, static_cast<int>(out[8]), "three quarter weight");
		assert_equal(200, static_cast<int>(out[12]), "last sample keeps the source end");
	}

	// A flat surface must survive any enlargement untouched, which a pair of weights not summing to
	// 256 would shift.
	{
		const auto src = make_source(7, 5);

		for (auto y = 0; y < 5; ++y)
		{
			auto* const row = src->pixels_line(y);

			for (auto x = 0; x < 7; ++x)
			{
				row[x * 4 + 0] = 19;
				row[x * 4 + 1] = 140;
				row[x * 4 + 2] = 233;
				row[x * 4 + 3] = 77;
			}
		}

		ui::surface_ptr dst;
		assert_equal(true, ui::bilinear_resize(src, dst, {31, 22}), "enlarge a flat surface");

		auto flat = true;

		for (auto y = 0; y < 22; ++y)
		{
			const auto* const row = dst->pixels_line(y);

			for (auto x = 0; x < 31; ++x)
			{
				flat = flat && row[x * 4 + 0] == 19 && row[x * 4 + 1] == 140 &&
					row[x * 4 + 2] == 233 && row[x * 4 + 3] == 77;
			}
		}

		assert_equal(true, flat, "an enlarged flat surface is unchanged");
	}

	// One axis growing while the other shrinks is neither a reduction nor an enlargement, so
	// area_downscale refuses it and this is what answers it.
	{
		const auto src = make_source(16, 16);
		ui::surface_ptr dst;
		assert_equal(false, ui::area_downscale(src, dst, {32, 8}), "the area filter refuses a mixed resize");
		assert_equal(true, ui::bilinear_resize(src, dst, {32, 8}), "bilinear takes a mixed resize");
		assert_equal(true, dst->dimensions() == sizei{32, 8}, "mixed resize dimensions");
	}

	// Planar surfaces are four bytes a pixel to this walk; they belong to swscale.
	{
		ui::surface nv12;
		nv12.alloc(8, 8, ui::texture_format::NV12);
		const auto nv12_view = ui::const_surface_ptr(&nv12, [](const ui::surface*)
		{
		});

		ui::surface_ptr dst;
		assert_equal(false, ui::bilinear_resize(nv12_view, dst, {16, 16}), "refuse a planar format");
	}
}

static void should_area_downscale_packed_and_planar_surfaces()
{
	const auto make_source = [](const int cx, const int cy, const ui::texture_format format)
	{
		auto s = std::make_shared<ui::surface>();
		s->alloc(cx, cy, format, ui::orientation::left_bottom);
		s->color_space(ui::color_space::rec709_limited);
		return s;
	};

	// A flat source must survive a reduction untouched. Any weight run that does not sum to exactly
	// 256 shifts the level here, which is the failure this whole scheme has to rule out.
	{
		const auto src = make_source(97, 61, ui::texture_format::ARGB);

		for (auto y = 0; y < 61; ++y)
		{
			auto* const row = src->pixels_line(y);
			for (auto x = 0; x < 97; ++x)
			{
				row[x * 4 + 0] = 17;
				row[x * 4 + 1] = 134;
				row[x * 4 + 2] = 251;
				row[x * 4 + 3] = 200;
			}
		}

		ui::surface_ptr dst;
		assert_equal(true, ui::area_downscale(src, dst, {13, 9}), "reduce a flat surface");
		assert_equal(true, dst->dimensions() == sizei{13, 9}, "reduced dimensions");
		assert_equal(true, dst->format() == ui::texture_format::ARGB, "reduced format");
		assert_equal(true, dst->orientation() == ui::orientation::left_bottom, "reduced orientation");
		assert_equal(true, dst->color_space() == ui::color_space::rec709_limited, "reduced colour space");

		for (auto y = 0; y < 9; ++y)
		{
			const auto* const row = dst->pixels_line(y);

			for (auto x = 0; x < 13; ++x)
			{
				assert_equal(17, static_cast<int>(row[x * 4 + 0]), "flat blue");
				assert_equal(134, static_cast<int>(row[x * 4 + 1]), "flat green");
				assert_equal(251, static_cast<int>(row[x * 4 + 2]), "flat red");
				assert_equal(200, static_cast<int>(row[x * 4 + 3]), "flat alpha");
			}
		}
	}

	// An exact 2:1 reduction must be the mean of each 2x2 block, so a checkerboard of 0 and 200
	// answers 100 everywhere. Nearest neighbour or a dropped contribution cannot produce that.
	{
		const auto src = make_source(64, 64, ui::texture_format::RGB);

		for (auto y = 0; y < 64; ++y)
		{
			auto* const row = src->pixels_line(y);
			for (auto x = 0; x < 64; ++x)
			{
				const auto v = static_cast<uint8_t>(((x + y) & 1) ? 200 : 0);
				row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = row[x * 4 + 3] = v;
			}
		}

		ui::surface_ptr dst;
		assert_equal(true, ui::area_downscale(src, dst, {32, 32}), "reduce a checkerboard");

		for (auto y = 0; y < 32; ++y)
		{
			const auto* const row = dst->pixels_line(y);
			for (auto x = 0; x < 32; ++x)
			{
				assert_equal(100, static_cast<int>(row[x * 4 + 1]), "checkerboard mean");
			}
		}
	}

	// A horizontal ramp reduced 4:1 must stay monotonic and keep its ends, which a wrong first index
	// or a truncated run would break.
	{
		const auto src = make_source(256, 8, ui::texture_format::RGB);

		for (auto y = 0; y < 8; ++y)
		{
			auto* const row = src->pixels_line(y);
			for (auto x = 0; x < 256; ++x)
			{
				row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = static_cast<uint8_t>(x);
				row[x * 4 + 3] = 255;
			}
		}

		ui::surface_ptr dst;
		assert_equal(true, ui::area_downscale(src, dst, {64, 2}), "reduce a ramp");

		const auto* const row = dst->pixels_line(0);
		assert_equal(2, static_cast<int>(row[0]), "ramp first sample");
		assert_equal(254, static_cast<int>(row[63 * 4]), "ramp last sample");

		auto monotonic = true;
		for (auto x = 1; x < 64; ++x) monotonic = monotonic && row[x * 4] > row[(x - 1) * 4];
		assert_equal(true, monotonic, "ramp stays monotonic");
	}

	// Refusals hand the work back to swscale rather than producing a wrong picture.
	{
		const auto rgb = make_source(8, 8, ui::texture_format::RGB);
		ui::surface_ptr dst;
		assert_equal(false, ui::area_downscale(rgb, dst, {16, 4}), "refuse an enlargement");
		assert_equal(false, ui::area_downscale(rgb, dst, {0, 4}), "refuse an empty extent");

		// P010 samples are 16 bit, which the Q8 accumulators cannot hold.
		ui::surface p010;
		p010.alloc(8, 8, ui::texture_format::P010);
		const auto p010_view = ui::const_surface_ptr(&p010, [](const ui::surface*)
		{
		});
		assert_equal(false, ui::area_downscale(p010_view, dst, {4, 4}), "refuse a 16 bit planar format");

		// NV12 chroma is one pair per 2x2 luma block, so an odd extent has no whole answer.
		ui::surface nv12;
		nv12.alloc(8, 8, ui::texture_format::NV12);
		const auto nv12_view = ui::const_surface_ptr(&nv12, [](const ui::surface*)
		{
		});
		assert_equal(false, ui::area_downscale(nv12_view, dst, {5, 4}), "refuse an odd nv12 extent");
	}

	// NV12 reduces as two planes: the luma on its own, and the interleaved chroma pair at half the
	// extent. A flat picture must survive both, and U must not bleed into V.
	{
		ui::surface nv12;
		nv12.alloc(64, 32, ui::texture_format::NV12);
		nv12.color_space(ui::color_space::rec601_full);

		const auto stride = nv12.stride();
		auto* const luma = nv12.pixels();
		auto* const chroma = luma + stride * 32;

		for (auto y = 0; y < 32; ++y)
		{
			auto* const row = luma + stride * y;
			for (auto x = 0; x < 64; ++x) row[x] = static_cast<uint8_t>(((x + y) & 1) ? 200 : 0);
		}

		for (auto y = 0; y < 16; ++y)
		{
			auto* const row = chroma + stride * y;
			for (auto x = 0; x < 32; ++x)
			{
				row[x * 2] = 90;
				row[x * 2 + 1] = 210;
			}
		}

		const auto nv12_view = ui::const_surface_ptr(&nv12, [](const ui::surface*)
		{
		});

		ui::surface_ptr dst;
		assert_equal(true, ui::area_downscale(nv12_view, dst, {32, 16}), "reduce an nv12 surface");
		assert_equal(true, dst->format() == ui::texture_format::NV12, "reduced nv12 format");
		assert_equal(true, dst->color_space() == ui::color_space::rec601_full, "reduced nv12 colour space");

		const auto dst_stride = dst->stride();
		auto luma_correct = true;

		for (auto y = 0; y < 16; ++y)
		{
			const auto* const row = dst->pixels() + dst_stride * y;
			for (auto x = 0; x < 32; ++x) luma_correct = luma_correct && row[x] == 100;
		}

		assert_equal(true, luma_correct, "nv12 luma is the 2x2 mean");

		auto chroma_correct = true;

		for (auto y = 0; y < 8; ++y)
		{
			const auto* const row = dst->pixels() + dst_stride * 16 + dst_stride * y;
			for (auto x = 0; x < 16; ++x)
			{
				chroma_correct = chroma_correct && row[x * 2] == 90 && row[x * 2 + 1] == 210;
			}
		}

		assert_equal(true, chroma_correct, "nv12 chroma keeps u and v apart");
	}

	// Same size on one axis is still a reduction; the identity run must not be rejected or shifted.
	{
		const auto src = make_source(40, 10, ui::texture_format::RGB);

		for (auto y = 0; y < 10; ++y)
		{
			auto* const row = src->pixels_line(y);
			for (auto x = 0; x < 40; ++x)
			{
				row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = row[x * 4 + 3] = static_cast<uint8_t>(
					(x == 20) ? 240 : 10);
			}
		}

		ui::surface_ptr dst;
		assert_equal(true, ui::area_downscale(src, dst, {40, 5}), "reduce one axis only");
		assert_equal(240, static_cast<int>(dst->pixels_line(0)[20 * 4]), "identity axis keeps the column");
		assert_equal(10, static_cast<int>(dst->pixels_line(0)[19 * 4]), "identity axis keeps its neighbour");
	}

	// The AVX2 and SSE2 accumulators must agree byte for byte, or a picture would depend on the CPU
	// it was reduced on. Sizes are deliberately not multiples of the vector widths so both the wide
	// loops and their scalar tails are covered.
	{
		const auto src = make_source(211, 97, ui::texture_format::ARGB);
		auto seed = 0x9e3779b9u;

		for (auto y = 0; y < 97; ++y)
		{
			auto* const row = src->pixels_line(y);

			for (auto x = 0; x < 211 * 4; ++x)
			{
				seed = seed * 1664525u + 1013904223u;
				row[x] = static_cast<uint8_t>(seed >> 24);
			}
		}

		for (const auto extent : {sizei{53, 29}, sizei{211, 12}, sizei{7, 97}, sizei{210, 96}})
		{
			ui::surface_ptr wide;
			ui::surface_ptr baseline;
			assert_equal(true, ui::area_downscale(src, wide, extent), "reduce with the selected width");
			assert_equal(true, ui::area_downscale_baseline(src, baseline, extent), "reduce with the SSE2 baseline");

			auto identical = true;

			for (auto y = 0; y < extent.cy; ++y)
			{
				identical = identical && memcmp(wide->pixels_line(y), baseline->pixels_line(y),
				                                static_cast<size_t>(extent.cx) * 4u) == 0;
			}

			assert_equal(true, identical, std::format("SIMD widths agree at {}x{}", extent.cx, extent.cy));
		}
	}
}

static void should_estimate_decode_cost()
{
	// The estimate reads only the header fields, but an image with no bytes counts as empty.
	const auto make_image = [](const ui::image_format format)
	{
		df::blob bytes;
		bytes.resize(16);
		return std::make_shared<ui::image>(std::move(bytes), sizei{4000, 3000}, format,
		                                   ui::orientation::top_left);
	};

	const auto png = make_image(ui::image_format::PNG);
	assert_equal(4000ll * 3000ll * 4, static_cast<uint64_t>(
		             files::estimate_decode_bytes(png, {400, 300})),
	             "a PNG builds the whole frame however small a target is asked for");

	const auto jpeg = make_image(ui::image_format::JPEG);
	assert_equal(500ll * 375ll * 4, static_cast<uint64_t>(
		             files::estimate_decode_bytes(jpeg, {400, 300})),
	             "libjpeg reduces by up to 1/8 while decoding");
	assert_equal(4000ll * 3000ll * 4, static_cast<uint64_t>(
		             files::estimate_decode_bytes(jpeg, {4000, 3000})),
	             "a full-size request costs a JPEG its whole frame");

	assert_equal(1000ll * 1000ll * 4, static_cast<uint64_t>(
		             files::estimate_decode_bytes(sizei{1000, 1000})),
	             "a bare source size costs four bytes a pixel");
}

static void should_refuse_over_budget_sources()
{
	const auto restore_budget = df::max_decode_bytes;
	const df::scope_exit restore([restore_budget] { df::max_decode_bytes = restore_budget; });

	df::max_decode_bytes = 64ll * 1024ll * 1024ll; // exactly 4096 x 4096

	load_diagnostic fits;
	assert_equal(false, reject_over_budget_source(&fits, {4096, 4096}, "test"), "a source at the budget is decoded");
	assert_equal(false, fits.over_budget, "a source that fits is not flagged");
	assert_equal(4096, fits.source_dimensions.cx, "the size is reported whether or not it fits");

	load_diagnostic refused;
	assert_equal(true, reject_over_budget_source(&refused, {4097, 4096}, "test"), "one column over is refused");
	assert_equal(true, refused.over_budget, "the refusal reaches the caller");
	assert_equal(4097, refused.source_dimensions.cx, "the refused size is reported so it can be shown");

	assert_equal(true, files::exceeds_decode_budget(sizei{8000, 8000}), "the predicate agrees with the loader");
	assert_equal(true, reject_over_budget_source(nullptr, {8000, 8000}, "test"),
	             "a caller that wants no diagnostic is still told to refuse");
}

// LibRaw builds the whole frame inside unpack/dcraw_process, so the budget has to be asked before
// that starts. RAW was the one source that allocated a full frame without asking, so a raw large
// enough to exhaust the machine walked straight past the ceiling every other decoder observes.
static void should_refuse_an_over_budget_raw()
{
	const auto path = test_files_folder.combine("raw").combine_file("Screws.CR2");

	const auto restore_budget = df::max_decode_bytes;
	const df::scope_exit restore([restore_budget] { df::max_decode_bytes = restore_budget; });

	files ff;

	// The preview path answers first and is not the subject, so it is asked for the full decode.
	const auto whole = ff.load(path, false);
	assert_equal(true, whole.success, "the fixture decodes within the default budget");

	const auto dimensions = whole.dimensions();
	assert_equal(false, dimensions.is_empty(), "and reports its size");

	// One pixel's worth under what this source needs.
	df::max_decode_bytes = static_cast<int64_t>(dimensions.cx) * dimensions.cy * 4 - 4;

	const auto refused = ff.load(path, false);
	assert_equal(false, refused.success, "a raw past the budget is refused");
	assert_equal(true, refused.reason == file_load_result::failure::too_large,
	             "and says so as a property of the file");
	assert_equal(false, is_valid(refused.s), "no frame was committed");
	assert_equal(false, is_valid(refused.i), "and no image either");
}

static void should_animate_alpha_between_values()
{
	// The gate is off when the CPU software backend is active, so force the animated
	// behaviour under test and restore whatever the running renderer selected.
	const auto restore_animations = ui::animations_enabled;
	const df::scope_exit restore_scope([restore_animations] { ui::animations_enabled = restore_animations; });
	ui::animations_enabled = true;

	ui::animate_alpha alpha;
	alpha.reset(0.0f, 1.0f);
	assert_equal(0.0f, alpha.val(), "fade-in starts at zero");
	assert_equal(1.0f, alpha.target(), "fade-in targets one");
	assert_equal(true, alpha.step(), "fade-in advances");
	assert_equal(true, alpha.val() > 0.0f && alpha.val() < 1.0f, "fade-in interpolates");

	alpha.target(0.0f);
	assert_equal(true, alpha.val() > 0.0f, "retarget preserves current value");
	assert_equal(true, alpha.step(), "fade-out advances");

	for (auto i = 0; i < 100; ++i) alpha.step();
	assert_equal(0.0f, alpha.val(), "fade-out reaches target");
	assert_equal(false, alpha.step(), "completed animation stops");

	alpha.reset(0.0005f, 0.0f);
	assert_equal(true, alpha.step(), "final snap requests a frame");
	assert_equal(0.0f, alpha.val(), "final snap reaches target");
	assert_equal(false, alpha.step(), "final snap completes once");
}

// A fade used to advance a fixed fraction per frame, so the same fade ran twice as fast on a 120Hz
// display as on a 60Hz one. It now advances by elapsed time, so equal wall-clock time must leave it
// in the same place whatever the refresh rate that produced it.
static void should_fade_at_the_same_rate_on_any_refresh_rate()
{
	const auto restore_animations = ui::animations_enabled;
	const auto restore_factor = ui::animation_step_factor;
	const df::scope_exit restore_scope([restore_animations, restore_factor]
	{
		ui::animations_enabled = restore_animations;
		ui::animation_step_factor = restore_factor;
	});
	ui::animations_enabled = true;

	const auto fade_for = [](const int frames_per_second, const double seconds)
	{
		const auto dt = 1.0 / frames_per_second;
		ui::animation_step_factor = static_cast<float>(1.0 - std::exp(-ui::alpha_fade_rate * dt));

		ui::animate_alpha alpha;
		alpha.reset(0.0f, 1.0f);

		for (auto i = 0; i < static_cast<int>(seconds * frames_per_second); ++i) alpha.step();

		return alpha.val();
	};

	constexpr auto quarter_second = 0.25;
	const auto at_60 = fade_for(60, quarter_second);
	const auto at_120 = fade_for(120, quarter_second);
	const auto at_30 = fade_for(30, quarter_second);

	assert_equal(true, at_60 > 0.1f && at_60 < 0.999f, "a quarter second is mid-fade");
	assert_equal(true, std::abs(at_120 - at_60) < 0.02f, "120Hz matches 60Hz at the same elapsed time");
	assert_equal(true, std::abs(at_30 - at_60) < 0.02f, "30Hz matches 60Hz at the same elapsed time");

	// The replaced constant: a 60Hz frame must still close a third of the remaining distance, so
	// nothing about the feel of a fade changes on the refresh rate almost every display reports.
	ui::animation_step_factor = static_cast<float>(1.0 - std::exp(-ui::alpha_fade_rate / 60.0));
	assert_equal(true, std::abs(ui::animation_step_factor - 0.333f) < 0.001f, "60Hz reproduces the old step");
}

static void should_skip_alpha_animation_when_disabled()
{
	// CPU software rendering cannot afford per-frame fades: every alpha reaches its target
	// immediately so draws stay on the opaque fast paths and no extra frames are requested.
	const auto restore_animations = ui::animations_enabled;
	const df::scope_exit restore_scope([restore_animations] { ui::animations_enabled = restore_animations; });
	ui::animations_enabled = false;

	ui::animate_alpha alpha;
	alpha.reset(0.0f, 1.0f);
	assert_equal(1.0f, alpha.val(), "fade-in starts complete");
	assert_equal(false, alpha.step(), "fade-in requests no frame");

	alpha.target(0.0f);
	assert_equal(0.0f, alpha.val(), "fade-out completes immediately");
	assert_equal(false, alpha.step(), "fade-out requests no frame");
}

static void should_resize()
{
	const auto save_path = _temps.next_path();
	const auto load_path = test_files_folder.combine_file("Test.jpg");

	image_edits edits;
	edits.scale(sizei(200, 150));

	files ff;
	ff.update(load_path, save_path, {}, edits, {}, false, {});

	const auto actual = extract_properties(save_path);

	if (!actual)
	{
		throw test_assert_exception(std::format("Should resize: could not extract properties from {}",
		                                        save_path.str()));
	}

	assert_equal(200, actual->width);
	assert_equal(133, actual->height);
}

// Issue #102 - Rotating an image (the transform behind the [ and ] shortcuts and
// the rotate_clockwise/anticlockwise commands). These tests validate the 90-degree
// rotation pipeline across JPEG (incl. lossless + EXIF-oriented), PNG and WebP;
// the keyboard-shortcut dispatch reported in #102 is a UI-level concern.
static void should_rotate()
{
	files ff;

	{
		const auto save_path = _temps.next_path();
		const auto load_path = test_files_folder.combine_file("Test.jpg");
		const auto loaded = ff.load(load_path, false);

		image_edits edits;
		const quadd crop(loaded.i->dimensions());
		edits.crop_bounds(crop.transform(simple_transform::rot_90));

		metadata_edits md_edits;
		md_edits.orientation = ui::orientation::top_left;

		ff.update(load_path, save_path, md_edits, edits, {}, false, {});

		const auto expected = extract_properties(test_files_folder.combine_file("Test90.jpg"));
		const auto actual = extract_properties(save_path);

		// Test.jpg is 683 pixels tall, which is not a whole number of MCUs, so the lossless path
		// declines it. The rotate must still keep every row rather than trimming to the MCU grid,
		// which the Test90.jpg fixture does not - it was captured from the old trimming path.
		expected->width = static_cast<int>(loaded.i->height());
		expected->height = static_cast<int>(loaded.i->width());

		assert_metadata(*expected, *actual);
	}

	{
		const auto save_path = _temps.next_path();
		const auto load_path = test_files_folder.combine_file("exif-rotated.jpg");
		const auto loaded = ff.load(load_path, false);

		image_edits edits;
		const quadd crop(loaded.i->dimensions());
		edits.crop_bounds(crop.transform(simple_transform::rot_90));

		metadata_edits md_edits;
		md_edits.orientation = ui::orientation::top_left;

		ff.update(load_path, save_path, md_edits, edits, {}, false, {});

		const auto actual_exif = extract_properties(save_path, metadata_type::EXIF);
		assert_equal(ui::orientation::top_left, actual_exif->orientation, "orientation");
	}

	{
		// PNG
		const auto save_path = _temps.next_path(".png");
		const auto load_path = test_files_folder.combine_file("engine.png");
		const auto loaded = ff.load(load_path, false);

		image_edits edits;
		const quadd crop(loaded.i->dimensions());
		edits.crop_bounds(crop.transform(simple_transform::rot_90));
		ff.update(load_path, save_path, {}, edits, {}, false, {});

		const auto updated = ff.load(save_path, false);
		assert_equal(loaded.i->height(), updated.i->width(), "png width");
		assert_equal(loaded.i->width(), updated.i->height(), "png height");
	}

	{
		// WEBP
		const auto save_path = _temps.next_path(".webp");
		const auto load_path = test_files_folder.combine_file("lake.webp");
		const auto loaded = ff.load(load_path, false);

		image_edits edits;
		const quadd crop(loaded.i->dimensions());
		edits.crop_bounds(crop.transform(simple_transform::rot_90));
		ff.update(load_path, save_path, {}, edits, {}, false, {});

		const auto updated = ff.load(save_path, false);
		assert_equal(false, updated.is_empty(), "webp result empty");
		assert_equal(loaded.i->height(), updated.i->width(), "webp width");
		assert_equal(loaded.i->width(), updated.i->height(), "webp height");
	}
}

static void should_not_cpu_transform_planar_surfaces()
{
	const auto planar = std::make_shared<ui::surface>();
	assert_equal(true, planar->alloc(8, 8, ui::texture_format::NV12), "nv12 surface allocates");

	const auto rotated = planar->transform(simple_transform::rot_90);
	assert_equal(false, ui::is_valid(rotated), "planar surfaces are not transformed as packed pixels");
}

static void should_rotate133()
{
	const auto save_path = _temps.next_path();
	const auto load_path = test_files_folder.combine_file("Test.jpg");

	files ff;
	const auto loaded = ff.load(load_path, false);

	const quadd crop(loaded.i->dimensions());
	image_edits edits;
	edits.crop_bounds(crop.rotate(133, crop.center_point()));

	ff.update(load_path, save_path, {}, edits, {}, false, {});

	const auto actual = extract_properties(save_path);
	const auto expected = expected_test_jpg();
	expected->width = 576;
	expected->height = 384;
	assert_metadata(*expected, *actual);
}

// The neutral parameter set is the one a picture with no edits is rendered through, so any drift in
// the tone curve, the YUV round trip or the rounding shows up here as a picture the app quietly
// altered. Truncating instead of rounding in adjust_color moves every channel a level and fails this.
static void should_leave_colour_unchanged_when_neutral()
{
	const auto src = std::make_shared<ui::surface>();
	assert_equal(true, src->alloc(16, 4, ui::texture_format::ARGB), "neutral source allocated");

	for (auto y = 0; y < 4; ++y)
	{
		auto* const row = src->pixels_line(y);

		for (auto x = 0; x < 16; ++x)
		{
			row[x * 4 + 0] = static_cast<uint8_t>(x * 17);
			row[x * 4 + 1] = static_cast<uint8_t>(255 - x * 17);
			row[x * 4 + 2] = static_cast<uint8_t>((x * 37 + y * 11) & 0xff);
			row[x * 4 + 3] = 255;
		}
	}

	const auto dst = std::make_shared<ui::surface>();
	assert_equal(true, dst->alloc(16, 4, ui::texture_format::ARGB), "neutral destination allocated");

	ui::color_adjust adjust;
	adjust.color_params(0, 0, 0, 0, 0, 0, 0);
	adjust.apply(src, dst->pixels(), dst->stride(), {});

	auto worst = 0;
	auto bias = 0;
	auto samples = 0;

	for (auto y = 0; y < 4; ++y)
	{
		const auto* const in = src->pixels_line(y);
		const auto* const out = dst->pixels_line(y);

		for (auto i = 0; i < 16 * 4; ++i)
		{
			const auto delta = static_cast<int>(out[i]) - static_cast<int>(in[i]);
			worst = std::max(worst, std::abs(delta));
			bias += delta;
			++samples;
		}
	}

	// One level of slack for the RGB -> YUV -> RGB round trip; two would hide a real curve error.
	assert_equal(true, worst <= 1, "neutral adjustment leaves the picture alone");

	// The residual must also be unbiased. A worst-case bound alone cannot see truncation, which costs
	// at most one level but costs it in the same direction on every pixel, darkening the whole picture.
	assert_equal(true, std::abs(bias) * 8 <= samples, "the neutral residual is not biased in one direction");
}

// The GPU preview and the saved result must agree: populate_texture_transform is a decimation of the
// same curve apply() uses, so a preview that does not track it shows the user one picture and commits
// another. The tolerance here is a level either side of the byte quantisation -- it catches a curve
// that is missing, stale, inverted or wrongly scaled, not a one-bin sampling offset.
static void should_match_the_preview_curve_to_the_applied_curve()
{
	// A neutral adjustment must leave the preview at the identity ramp the transform is born with,
	// or an unedited picture is previewed through a curve.
	{
		ui::color_adjust neutral;
		neutral.color_params(0, 0, 0, 0, 0, 0, 0);

		ui::texture_transform transform;
		const ui::texture_transform identity;
		neutral.populate_texture_transform(transform);

		auto worst = 0.0f;

		for (auto index = 0; index < ui::texture_transform::curve_len; ++index)
		{
			worst = std::max(worst, std::abs(transform.curve[index] - identity.curve[index]));
		}

		assert_equal(true, worst < 0.01f, "a neutral adjustment previews as the identity curve");
	}

	ui::color_adjust adjust;
	adjust.color_params(0, 0, 0.5, 0.0, -0.5, 0.4, 0.1);

	ui::texture_transform transform;
	adjust.populate_texture_transform(transform);

	assert_equal(true, transform.has_color_changes, "an adjusted curve reports colour changes");

	// It must also no longer be the identity, which is what a preview that was never repopulated
	// would still be showing.
	{
		const ui::texture_transform identity;
		assert_equal(false, transform.curve == identity.curve, "an adjusted curve is not the identity");
	}

	// A grey ramp isolates the curve: with no saturation change the output luma is the curve itself.
	const auto src = std::make_shared<ui::surface>();
	assert_equal(true, src->alloc(ui::texture_transform::curve_len, 1, ui::texture_format::ARGB),
	             "ramp source allocated");

	auto* const row = src->pixels_line(0);

	for (auto x = 0; x < ui::texture_transform::curve_len; ++x)
	{
		row[x * 4 + 0] = static_cast<uint8_t>(x);
		row[x * 4 + 1] = static_cast<uint8_t>(x);
		row[x * 4 + 2] = static_cast<uint8_t>(x);
		row[x * 4 + 3] = 255;
	}

	const auto dst = std::make_shared<ui::surface>();
	assert_equal(true, dst->alloc(ui::texture_transform::curve_len, 1, ui::texture_format::ARGB),
	             "ramp destination allocated");
	adjust.apply(src, dst->pixels(), dst->stride(), {});

	const auto* const out = dst->pixels_line(0);
	auto worst = 0;

	for (auto x = 0; x < ui::texture_transform::curve_len; ++x)
	{
		const auto previewed = df::round(transform.curve[x] * 255.0);
		worst = std::max(worst, std::abs(previewed - static_cast<int>(out[x * 4 + 1])));
	}

	assert_equal(true, worst <= 2, "the preview curve tracks the curve apply uses");

	// Monotonic, because a tone curve that folds back inverts contrast somewhere in the picture.
	for (auto x = 1; x < ui::texture_transform::curve_len; ++x)
	{
		assert_equal(true, transform.curve[x] >= transform.curve[x - 1], "the tone curve never folds back");
	}

	// MEDIA-001 - raw BGRA primaries catch a saved-path red/blue swap that a grey ramp cannot.
	{
		ui::color_adjust desaturate;
		desaturate.color_params(0, -1.0, 0, 0, 0, 0, 0);

		const auto primaries = std::make_shared<ui::surface>();
		assert_equal(true, primaries->alloc(3, 1, ui::texture_format::ARGB), "primary source allocated");
		auto* const primary_row = primaries->pixels_line(0);
		const std::array<std::array<uint8_t, 4>, 3> bgra_pixels{{
			{0, 0, 255, 255}, // red in BGRA memory order
			{0, 255, 0, 255},
			{255, 0, 0, 255}
		}};
		for (auto x = 0; x < 3; ++x) std::memcpy(primary_row + x * 4, bgra_pixels[x].data(), 4);

		const auto primary_out = std::make_shared<ui::surface>();
		assert_equal(true, primary_out->alloc(3, 1, ui::texture_format::ARGB), "primary destination allocated");
		assert_equal(true, desaturate.apply(primaries, primary_out->pixels(), primary_out->stride(), {}),
		             "primary desaturation applied");

		const auto* const desaturated = primary_out->pixels_line(0);
		const std::array<int, 3> expected_luma{76, 150, 29};
		for (auto x = 0; x < 3; ++x)
		{
			const auto b = static_cast<int>(desaturated[x * 4 + 0]);
			const auto g = static_cast<int>(desaturated[x * 4 + 1]);
			const auto r = static_cast<int>(desaturated[x * 4 + 2]);
			assert_equal(true, std::abs(r - expected_luma[x]) <= 1, "primary red-channel luminance");
			assert_equal(true, std::abs(g - expected_luma[x]) <= 1, "primary green-channel luminance");
			assert_equal(true, std::abs(b - expected_luma[x]) <= 1, "primary blue-channel luminance");
		}
	}

	// MEDIA-001 - temperature is channel-directed: warming a red pixel should not boost the stored
	// blue byte, and cooling a blue pixel should not boost the stored red byte.
	{
		ui::color_adjust warm;
		warm.color_params(0, 0, 0, 0, 0, 0, 0, 1.0, 0);

		const auto src_warm = std::make_shared<ui::surface>();
		assert_equal(true, src_warm->alloc(2, 1, ui::texture_format::ARGB), "temperature source allocated");
		auto* const row_warm = src_warm->pixels_line(0);
		const std::array<std::array<uint8_t, 4>, 2> bgra_pixels{{
			{10, 20, 160, 255},
			{160, 20, 10, 255}
		}};
		for (auto x = 0; x < 2; ++x) std::memcpy(row_warm + x * 4, bgra_pixels[x].data(), 4);

		const auto dst_warm = std::make_shared<ui::surface>();
		assert_equal(true, dst_warm->alloc(2, 1, ui::texture_format::ARGB), "temperature destination allocated");
		assert_equal(true, warm.apply(src_warm, dst_warm->pixels(), dst_warm->stride(), {}),
		             "temperature adjustment applied");

		const auto* const out_warm = dst_warm->pixels_line(0);
		assert_equal(true, out_warm[2] > row_warm[2], "warming raises red for a red-biased pixel");
		assert_equal(true, out_warm[0] < row_warm[0], "warming lowers blue for a blue-biased pixel");
	}
}

// Full desaturation is the one adjustment with an answer that can be stated exactly, so it pins the
// chroma path independently of the tone curve.
static void should_desaturate_to_grey()
{
	const auto src = std::make_shared<ui::surface>();
	assert_equal(true, src->alloc(8, 1, ui::texture_format::ARGB), "chroma source allocated");

	auto* const row = src->pixels_line(0);

	for (auto x = 0; x < 8; ++x)
	{
		row[x * 4 + 0] = static_cast<uint8_t>(x * 30);
		row[x * 4 + 1] = static_cast<uint8_t>(200 - x * 20);
		row[x * 4 + 2] = static_cast<uint8_t>(40 + x * 25);
		row[x * 4 + 3] = 255;
	}

	const auto dst = std::make_shared<ui::surface>();
	assert_equal(true, dst->alloc(8, 1, ui::texture_format::ARGB), "chroma destination allocated");

	ui::color_adjust adjust;
	adjust.color_params(0, -1.0, 0, 0, 0, 0, 0);
	adjust.apply(src, dst->pixels(), dst->stride(), {});

	const auto* const out = dst->pixels_line(0);

	for (auto x = 0; x < 8; ++x)
	{
		const auto b = static_cast<int>(out[x * 4 + 0]);
		const auto g = static_cast<int>(out[x * 4 + 1]);
		const auto r = static_cast<int>(out[x * 4 + 2]);

		assert_equal(true, std::abs(r - g) <= 1 && std::abs(g - b) <= 1,
		             "saturation of -1 leaves every pixel grey");
		assert_equal(255u, static_cast<uint32_t>(out[x * 4 + 3]), "alpha survives desaturation");
	}
}

static void should_rasterise_tiles_identically_to_the_whole_surface()
{
	const auto probe = ui::probe_software_rasterizer();

	assert_equal(true, probe.tiles > 1, "the scene was split across tiles");
	assert_equal(true, probe.painted_pixels > 1000, "the scene painted something to compare");
	assert_equal(0, probe.mismatched_pixels, "tiled rasterisation matches the whole surface");
}

// The backend parity contract says the same scene is the same picture everywhere, and nothing
// checked that across a machine: probe_software_rasterizer compares two renderings inside one
// process, which cannot see one platform disagreeing with another. These samples are that artifact.
//
// Compared within a tolerance, not hashed. An exact digest of this scene is stable across MSVC and
// GCC but *not* across MSVC Debug and Release, because the rasterizer is float arithmetic an
// optimiser may reassociate - so an exact comparison fails for a reason this code does not own. A
// structural break, which is what the gate is for, moves a sample far further than a rounding step:
// a transposed, mirrored or channel-swapped blit, or a blend against the wrong background, changes
// whole channels.
static void should_rasterise_one_scene_to_one_answer_on_every_platform()
{
	// Recorded 2026-08-16 from MSVC x64 Debug, and holding unchanged under MSVC x64 Release and
	// GCC 13 x86-64 in both configurations. Re-record from a run on both platforms when the scene
	// or a primitive changes.
	// The first and last samples are the untouched fill, which is how the scene is pinned to
	// painting only inside itself.
	static constexpr std::array<uint32_t, ui::rasterizer_sample_count> recorded{
		0x40404040, 0xff543e8b, 0xff3366e6, 0xff8344a1, 0xff8344a1, 0xffe6d972,
		0xff4e4b8e, 0xff2d3d7c, 0xff6ba8a5, 0xff719d69, 0xff6c7561, 0xff869a5a,
		0xff5e954b, 0xff56a764, 0xff406e73, 0xff26377e, 0xff838387, 0xff5b8773,
		0xff3bbe71, 0xff84a37b, 0xff9b888c, 0xff40703f, 0xff5c7440, 0x40404040,
	};

	constexpr int tolerance = 3;

	const auto probe = ui::probe_software_rasterizer();

	const auto format = [](const std::array<uint32_t, ui::rasterizer_sample_count>& s)
	{
		std::string result;
		for (const auto v : s) result += std::format("0x{:08x}, ", v);
		return result;
	};

	auto within = true;

	for (auto i = 0; i < ui::rasterizer_sample_count; ++i)
	{
		for (auto shift = 0; shift < 32; shift += 8)
		{
			const auto expected = static_cast<int>((recorded[i] >> shift) & 0xff);
			const auto actual = static_cast<int>((probe.samples[i] >> shift) & 0xff);
			if (std::abs(expected - actual) > tolerance) within = false;
		}
	}

	// Compared as text only once a sample is out of tolerance, so the failure carries the whole
	// vector and can be pasted back as the new record rather than reassembled a channel at a time.
	assert_equal(format(recorded), within ? format(recorded) : format(probe.samples),
	             "the reference scene rasterises to the recorded pixels");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The globe rasteriser
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	// An equirectangular world stand-in whose colour says where a pixel came from: everything above
	// 45 north is green, the western hemisphere red, the eastern blue.
	ui::const_surface_ptr make_globe_test_source()
	{
		auto source = std::make_shared<ui::surface>();
		source->alloc(256, 128, ui::texture_format::ARGB);

		for (auto y = 0; y < 128; ++y)
		{
			auto* const line = std::bit_cast<ui::color32*>(source->pixels_line(y));

			for (auto x = 0; x < 256; ++x)
			{
				line[x] = y < 32
					          ? ui::rgba(0, 255, 0)
					          : x < 128
					          ? ui::rgba(255, 0, 0)
					          : ui::rgba(0, 0, 255);
			}
		}

		return source;
	}

	// Red at full brightness with a triangular ramp in blue. Shading scales both channels by the
	// same factor, so blue/red recovers where a rendered pixel sampled from, whatever the light did
	// to it. The ramp is a triangle rather than a saw so that it is continuous across the date line
	// too - a saw would jump 255 to 0 there and every reading near the seam would be a blend of the
	// two ends rather than a position.
	ui::const_surface_ptr make_globe_ramp_source()
	{
		auto source = std::make_shared<ui::surface>();
		source->alloc(256, 128, ui::texture_format::ARGB);

		for (auto y = 0; y < 128; ++y)
		{
			auto* const line = std::bit_cast<ui::color32*>(source->pixels_line(y));

			for (auto x = 0; x < 256; ++x)
			{
				const auto ramp = static_cast<uint32_t>(x < 128 ? x * 2 : (255 - x) * 2);
				line[x] = ui::rgba(255, 0, ramp);
			}
		}

		return source;
	}

	// Which of the source's three colours a rendered pixel came from, whatever the shading did to
	// its brightness. '-' when nothing is drawn there.
	char globe_hue(const ui::color32 c)
	{
		if (ui::get_a(c) < 128) return '-';

		const auto r = ui::get_r(c);
		const auto g = ui::get_g(c);
		const auto b = ui::get_b(c);

		if (r > g && r > b) return 'r';
		if (g > r && g > b) return 'g';
		if (b > r && b > g) return 'b';
		return '?';
	}
}

static void should_render_the_globe()
{
	globe_renderer renderer;
	renderer.set_source(make_globe_test_source());
	assert_equal(true, renderer.is_ready(), "the renderer keeps the world it was given", "globe render");

	constexpr int radius = 100;
	constexpr pointi center(104, 104);

	ui::surface destination;
	destination.alloc(208, 208, ui::texture_format::ARGB);

	const auto render_view = [&](const gps_coordinate view)
	{
		return renderer.render(destination, globe_projection(view, center, radius));
	};

	assert_equal(true, render_view({0.0, -90.0}), "a view renders", "globe render");
	assert_equal('r', globe_hue(destination.get_pixel(center.x, center.y)),
	             "the western hemisphere holds the centre", "globe render");

	assert_equal(true, render_view({0.0, 90.0}), "the opposite view renders", "globe render");
	assert_equal('b', globe_hue(destination.get_pixel(center.x, center.y)), "and so does the eastern",
	             "globe render");

	assert_equal(true, render_view({60.0, 0.0}), "a northern view renders", "globe render");
	assert_equal('g', globe_hue(destination.get_pixel(center.x, center.y)), "the north faces the viewer",
	             "globe render");

	// Off the sphere is nothing at all, and the limb is neither hard nor missing.
	assert_equal(0u, ui::get_a(destination.get_pixel(center.x + radius + 4, center.y)),
	             "beyond the sphere is transparent", "globe render");
	assert_equal(255u, ui::get_a(destination.get_pixel(center.x, center.y)), "the centre is opaque",
	             "globe render");

	auto feathered = false;

	for (auto x = center.x + radius - 3; x <= center.x + radius + 1; ++x)
	{
		const auto alpha = ui::get_a(destination.get_pixel(x, center.y));
		if (alpha > 0 && alpha < 255) feathered = true;
	}

	assert_equal(true, feathered, "the limb is antialiased rather than a staircase", "globe render");

	// Where the hemispheres meet says the projection reached the rasteriser: at a view 45 degrees
	// west of the meridian the seam is drawn at sin(45) of the radius, east of centre.
	const auto seam_at = [&](const gps_coordinate view)
	{
		render_view(view);
		auto previous = globe_hue(destination.get_pixel(center.x - radius + 2, center.y));
		auto seam = -1000;
		auto seams = 0;

		for (auto x = center.x - radius + 3; x <= center.x + radius - 2; ++x)
		{
			const auto hue = globe_hue(destination.get_pixel(x, center.y));
			if (hue == '-' || hue == '?') continue;
			if (hue != previous)
			{
				++seams;
				seam = x - center.x;
			}
			previous = hue;
		}

		return std::pair{seam, seams};
	};

	const auto [meridian, meridian_seams] = seam_at({0.0, 0.0});
	assert_equal(1, meridian_seams, "the meridian is crossed once", "globe render");
	assert_near(0.0, meridian, 2.0, "and sits at the centre", "globe render");

	const auto [offset, offset_seams] = seam_at({0.0, -45.0});
	assert_equal(1, offset_seams, "an offset view still crosses once", "globe render");
	assert_near(radius * 0.7071, offset, 3.0, "at the sine of the turn", "globe render");

	// The date line is where interpolating longitude across a segment goes wrong: a segment that
	// straddles it and is not unwrapped sweeps the whole map backwards. Note the view is 179.9 and
	// not 180: gps_coordinate spends exactly 180 as its "no coordinate" sentinel, and a projection
	// handed one falls back to the meridian - which would test nothing at all here.
	const gps_coordinate date_line_view(0.0, 179.9);

	const auto [date_line, date_line_seams] = seam_at(date_line_view);
	assert_equal(1, date_line_seams, "the date line is crossed once, not many times", "globe render");
	assert_near(0.0, date_line, 2.0, "and it too sits where the view says", "globe render");

	globe_renderer ramp;
	ramp.set_source(make_globe_ramp_source());
	assert_equal(true, ramp.render(destination, globe_projection(date_line_view, center, radius)),
	             "a world that says where each pixel came from renders", "globe render");

	// Away from the limb the view turns about half a degree per pixel, so the ramp read off the
	// pixels moves by about one step at a time. A segment interpolated without unwrapping sweeps
	// the whole map inside sixteen pixels, so its readings jump by tens - which is a defect of the
	// sampling itself, and neither a hue nor a run length can see it.
	// Away from the limb the view turns about half a degree per pixel, so the ramp read off the
	// pixels moves about a step at a time; measured, the largest step is 1.4. A segment interpolated
	// without unwrapping sweeps the whole map inside sixteen pixels and that becomes 32, which is a
	// defect of the sampling itself that neither a hue nor a run length can see.
	auto jumps = 0;
	auto previous = -1.0;

	for (auto x = center.x - radius / 2; x <= center.x + radius / 2; ++x)
	{
		const auto c = destination.get_pixel(x, center.y);
		const auto red = static_cast<double>(ui::get_r(c));
		if (ui::get_a(c) != 255 || red < 16.0) continue;

		const auto position = 255.0 * ui::get_b(c) / red;
		if (previous >= 0.0 && std::abs(position - previous) > 8.0) ++jumps;
		previous = position;
	}

	assert_equal(0, jumps, "the row samples the map continuously across the date line", "globe render");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The panorama rasteriser
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	// An equirectangular source whose colour says which quadrant of itself a pixel came from, so a
	// rendered pixel can be traced back to a longitude and a latitude rather than only to "some
	// pixel". Green is the northern half; red and blue split the southern half west and east.
	ui::const_surface_ptr make_panorama_test_source()
	{
		auto source = std::make_shared<ui::surface>();
		source->alloc(512, 256, ui::texture_format::ARGB);

		for (auto y = 0; y < 256; ++y)
		{
			auto* const line = std::bit_cast<ui::color32*>(source->pixels_line(y));

			for (auto x = 0; x < 512; ++x)
			{
				line[x] = y < 128
					          ? ui::rgba(0, 255, 0)
					          : x < 256
					          ? ui::rgba(255, 0, 0)
					          : ui::rgba(0, 0, 255);
			}
		}

		return source;
	}

	char panorama_hue(const ui::color32 c)
	{
		if (ui::get_a(c) < 128) return '-';

		const auto r = ui::get_r(c);
		const auto g = ui::get_g(c);
		const auto b = ui::get_b(c);

		if (r > g && r > b) return 'r';
		if (g > r && g > b) return 'g';
		if (b > r && b > g) return 'b';
		return '?';
	}
}

static void should_render_a_projected_panorama()
{
	panorama_renderer renderer;
	renderer.set_source(make_panorama_test_source());
	assert_equal(true, renderer.is_ready(), "the renderer keeps the source it was given", "panorama render");

	const prop::panorama_geometry sphere{512, 256, 0, 0, 512, 256};

	ui::surface destination;
	destination.alloc(240, 160, ui::texture_format::ARGB);

	constexpr auto middle_x = 120;
	constexpr auto middle_y = 80;

	panorama_view view;
	view.reset(sphere);
	assert_equal(true, renderer.render(destination, sphere, view), "a view renders", "panorama render");

	// Facing the middle of the sphere is facing the boundary of the two southern quadrants, so the
	// pixel just left of centre is western and the one just right of it is eastern. An asymmetric
	// probe: "both halves appear" would pass for a mirrored projection too.
	assert_equal('r', panorama_hue(destination.get_pixel(middle_x - 8, middle_y + 40)),
	             "west of the centre is the western quadrant", "panorama render");
	assert_equal('b', panorama_hue(destination.get_pixel(middle_x + 8, middle_y + 40)),
	             "and east of it is the eastern", "panorama render");
	assert_equal('g', panorama_hue(destination.get_pixel(middle_x, middle_y - 40)),
	             "above the horizon is the northern half", "panorama render");

	// Turning east brings the eastern quadrant under the centre. A quarter turn is unambiguous:
	// nothing else in the source is blue on both sides of the middle.
	view.look_at(M_PI / 2.0, 0.0);
	assert_equal(true, renderer.render(destination, sphere, view), "a turned view renders", "panorama render");
	assert_equal('b', panorama_hue(destination.get_pixel(middle_x - 8, middle_y + 40)),
	             "a quarter turn east puts the eastern quadrant under the whole centre", "panorama render");
	assert_equal('b', panorama_hue(destination.get_pixel(middle_x + 8, middle_y + 40)),
	             "on both sides of it", "panorama render");

	// Looking up fills the frame with the northern half, including below the middle, which the
	// level view never does.
	view.look_at(0.0, M_PI / 2.0);
	assert_equal(true, renderer.render(destination, sphere, view), "an upward view renders", "panorama render");
	assert_equal('g', panorama_hue(destination.get_pixel(middle_x, middle_y + 40)),
	             "looking up is the northern half all the way down the frame", "panorama render");

	// A partial panorama ends. Its sky is not stretched to the top of the frame and it is not the
	// nearest row repeated - it is nothing, which is the honest answer and the one the flat draw
	// could not give.
	const auto strip = prop::panorama_geometry::assumed({512, 64});
	auto strip_source = std::make_shared<ui::surface>();
	strip_source->alloc(512, 64, ui::texture_format::ARGB);
	for (auto y = 0; y < 64; ++y)
	{
		auto* const line = std::bit_cast<ui::color32*>(strip_source->pixels_line(y));
		for (auto x = 0; x < 512; ++x) line[x] = ui::rgba(255, 0, 0);
	}

	panorama_renderer partial;
	partial.set_source(strip_source);

	panorama_view strip_view;
	strip_view.reset(strip);
	assert_equal(true, partial.render(destination, strip, strip_view), "a partial panorama renders",
	             "panorama render");
	assert_equal('r', panorama_hue(destination.get_pixel(middle_x, middle_y)),
	             "the strip itself is drawn on the horizon", "panorama render");
	assert_equal('-', panorama_hue(destination.get_pixel(middle_x, 2)),
	             "and above it is sphere the file does not hold", "panorama render");
	assert_equal('-', panorama_hue(destination.get_pixel(middle_x, 157)),
	             "as is below it", "panorama render");

	// The drawn area must be the covered area, column by column. A screen row is a curve in
	// latitude, so a renderer that decides coverage once per segment paints across the edge where
	// the curve bulges out and bites lumps out of it where it bulges in - both quantised to the
	// segment width, which is what makes the edge read as ragged instead of as an edge. One pixel
	// of tolerance for the boundary itself; anything beyond that is a segment-sized error.
	const auto check_edges = [&](const panorama_view& probe, const std::string_view what)
	{
		const auto dims = destination.dimensions();
		auto over = 0;
		auto under = 0;

		for (auto x = 0; x < dims.cx; ++x)
		{
			auto first = -1;
			auto last = -2;

			for (auto y = 0; y < dims.cy; ++y)
			{
				double u, v;
				if (!probe.texel_at(x + 0.5, y + 0.5, sized(dims), strip, u, v)) continue;
				if (first < 0) first = y;
				last = y;
			}

			for (auto y = 0; y < dims.cy; ++y)
			{
				const auto drawn = ui::get_a(destination.get_pixel(x, y)) >= 128;
				if (drawn && (y < first - 1 || y > last + 1)) ++over;
				if (!drawn && y > first && y < last) ++under;
			}
		}

		assert_equal(0, over, std::format("{}: nothing is drawn outside the file's own band", what),
		             "panorama render");
		assert_equal(0, under, std::format("{}: nothing inside it is left out", what), "panorama render");
	};

	check_edges(strip_view, "level");

	// Pitched, where the row curve is at its most bowed and a per-segment decision is most wrong.
	strip_view.look_at(0.0, 12.0 * M_PI / 180.0);
	assert_equal(true, partial.render(destination, strip, strip_view), "a pitched partial renders",
	             "panorama render");
	check_edges(strip_view, "pitched");

	// The decode hands out planar YUV unless something says it will read the pixels itself. Walked
	// as colour values a luma plane is grey and tiles, and it fails looking like a broken projection
	// rather than like a surface that cannot be read - so the renderer refuses it outright.
	auto planar = std::make_shared<ui::surface>();
	planar->alloc(512, 256, ui::texture_format::NV12);

	panorama_renderer yuv;
	yuv.set_source(planar);
	assert_equal(false, yuv.is_ready(), "a planar source is refused rather than misread", "panorama render");
	assert_equal(false, yuv.render(destination, sphere, view), "and renders nothing", "panorama render");
}

static void should_scale_month_blocks_logarithmically()
{
	assert_equal(0.0, chart_log_height(0, 1000), "an empty month has no block", "chart height");
	assert_equal(1.0, chart_log_height(1000, 1000), "the fullest month fills the row", "chart height");

	// The point of the log: a month holding a hundredth of the busiest month still earns a third of
	// the height. Scaled linearly it would be a line the user cannot see, let alone judge.
	const auto hundredth = chart_log_height(10, 1000);
	assert_equal(true, hundredth > 0.3, "a quiet month stays visible beside a holiday month", "chart height");
	assert_equal(true, hundredth < 1.0, "and still reads as the smaller of the two", "chart height");

	auto monotonic = true;
	auto previous = -1.0;

	for (uint64_t count = 0; count <= 1000; count += 7)
	{
		const auto height = chart_log_height(count, 1000);
		if (height < previous) monotonic = false;
		previous = height;
	}

	assert_equal(true, monotonic, "more items are never shorter", "chart height");
}

static void should_point_at_the_pie_wedge_that_was_drawn()
{
	const auto build = [](const bool raise_back, chart_surface& chart)
	{
		assert_equal(true, chart.prepare({160, 160}), "prepare the pie surface", "pie chart");

		pie_chart_scene scene;

		// Two halves, two colours, two identities. Segment zero is the back of the tilted disc,
		// which is where the largest media type is placed, so a pixel that reports the wrong half
		// would put the wrong search behind a click.
		for (auto i = 0; i < pie_chart_segment_count; ++i)
		{
			const auto back = i < pie_chart_segment_count / 2;
			scene.wedges[i] = {
				back ? ui::rgba(255, 0, 0, 255) : ui::rgba(0, 0, 255, 255),
				static_cast<uint16_t>(back ? pie_chart_wedge_id_base : pie_chart_wedge_id_base + 1),
				back && raise_back
			};
		}

		scene.hole_color = ui::rgba(0, 255, 0, 255);
		render_pie_chart(chart, scene);
	};

	chart_surface chart;
	build(false, chart);

	const auto extent = chart.extent();

	assert_equal(static_cast<int>(pie_chart_hole_id),
	             static_cast<int>(chart.id_at({extent.cx / 2, extent.cy / 2})),
	             "the well floor is what the centre of the chart points at", "pie chart");

	auto back_pixels = 0;
	auto front_pixels = 0;
	auto mismatches = 0;
	auto back_top = extent.cy;

	for (auto y = 1; y < extent.cy - 1; ++y)
	{
		for (auto x = 1; x < extent.cx - 1; ++x)
		{
			const auto id = chart.id_at({x, y});
			if (id < pie_chart_wedge_id_base) continue;

			const auto back = id == pie_chart_wedge_id_base;
			if (back) back_top = std::min(back_top, y);

			// Only pixels the reduction did not mix, so this measures the identity buffer rather
			// than the antialiasing along the seam between the halves.
			if (chart.id_at({x - 1, y}) != id || chart.id_at({x + 1, y}) != id ||
				chart.id_at({x, y - 1}) != id || chart.id_at({x, y + 1}) != id)
			{
				continue;
			}

			const auto c = chart.pixels()->get_pixel(x, y);

			if (back)
			{
				++back_pixels;
				if (ui::get_r(c) <= ui::get_b(c)) ++mismatches;
			}
			else
			{
				++front_pixels;
				if (ui::get_b(c) <= ui::get_r(c)) ++mismatches;
			}
		}
	}

	assert_equal(true, back_pixels > 200, "the back half is drawn", "pie chart");
	assert_equal(true, front_pixels > 200, "the front half is drawn", "pie chart");
	assert_equal(0, mismatches, "every identity names the wedge whose colour that pixel is", "pie chart");

	chart_surface raised;
	build(true, raised);

	auto raised_back_top = extent.cy;

	for (auto y = 0; y < extent.cy; ++y)
	{
		for (auto x = 0; x < extent.cx; ++x)
		{
			if (raised.id_at({x, y}) == pie_chart_wedge_id_base) raised_back_top = std::min(raised_back_top, y);
		}
	}

	assert_equal(true, raised_back_top < back_top, "a hovered media type lifts out of the disc", "pie chart");
}

static void should_reach_every_month_block_in_the_calendar()
{
	const sizei extent(240, 20);
	const calendar_chart_style style{3, 2, 4, 2};

	const auto build = [extent, style](const bool raise, chart_surface& chart)
	{
		assert_equal(true, chart.prepare(extent), "prepare the calendar surface", "calendar chart");

		std::vector<calendar_chart_cell> cells;

		for (auto m = 0; m < 12; ++m)
		{
			// The last month stands for one the collection has not reached: a socket with no
			// identity, so it is drawn but cannot be aimed at.
			const auto future = m == 11;
			cells.emplace_back(recti(df::mul_div(m, extent.cx, 12), 0, df::mul_div(m + 1, extent.cx, 12), extent.cy),
			                   future ? 0 : m,
			                   ui::rgba(200, 200, 200, 255),
			                   future ? chart_surface::no_id : static_cast<uint16_t>(m + 1),
			                   raise && m == 5);
		}

		render_calendar_chart(chart, cells, style);
		return cells;
	};

	chart_surface chart;
	const auto cells = build(false, chart);

	std::array<int, 13> found{};
	std::array<int, 13> top{};
	std::ranges::fill(top, extent.cy);
	auto strays = 0;
	auto future_pixels = 0;

	const auto& future_cell = cells[11].cell;

	for (auto y = 0; y < extent.cy; ++y)
	{
		for (auto x = 0; x < extent.cx; ++x)
		{
			const auto id = chart.id_at({x, y});
			if (id == chart_surface::no_id) continue;

			found[id] += 1;
			top[id] = std::min(top[id], y);

			// design.md targeting: a block may lean into the column to its right by its own depth
			// and no further, or the pointer selects a month it is not over.
			const auto& cell = cells[id - 1].cell;
			if (x < cell.left || x > cell.right + style.depth_x) ++strays;
			if (x >= future_cell.left + style.depth_x) ++future_pixels;
		}
	}

	for (auto m = 0; m < 11; ++m)
	{
		assert_equal(true, found[m + 1] > 0, "every month the collection covers can be pointed at", "calendar chart");
	}

	assert_equal(0, strays, "no block claims a pixel outside its own column", "calendar chart");
	assert_equal(0, future_pixels, "a month the collection has not reached cannot be aimed at", "calendar chart");

	// Height is what carries the count, so the busiest month has to stand taller than a quiet one.
	assert_equal(true, top[11] < top[2], "a busier month rises further", "calendar chart");

	chart_surface raised;
	build(true, raised);

	auto raised_top = extent.cy;

	for (auto y = 0; y < extent.cy; ++y)
	{
		for (auto x = 0; x < extent.cx; ++x)
		{
			if (raised.id_at({x, y}) == 6) raised_top = std::min(raised_top, y);
		}
	}

	assert_equal(true, raised_top < top[6], "a hovered month floats above the row", "calendar chart");
}

static void should_keep_an_overlapped_month_selectable()
{
	// A block tall enough to clear its own row leans over the row above. That is only allowed
	// because the sidebar cuts it short of the block above, so the month it leans over keeps a band
	// of its own to be pointed at. This is the contract that clamp has to honour, and the second
	// pass here is the negative control: without the clamp the month above is simply gone.
	const sizei extent(60, 32);
	const calendar_chart_style style{2, 2, 3, 2};
	constexpr auto row_height = 16;
	constexpr auto depth_y = 2;
	constexpr auto min_exposed = 4;

	// The upper block tops out here, so the lower one may rise no further than min_exposed below
	// it: (31 - depth_y) - 9 - min_exposed.
	constexpr auto upper_height = 4;
	constexpr auto upper_top = row_height - 1 - upper_height - depth_y;
	constexpr auto clamped = row_height * 2 - 1 - depth_y - upper_top - min_exposed;

	const auto reachable = [&](const int lower_height)
	{
		chart_surface chart;
		assert_equal(true, chart.prepare(extent), "prepare the calendar surface", "calendar overlap");

		// Cells are given in drawing order: the upper row first, so the block below occludes it.
		const std::vector<calendar_chart_cell> cells{
			{recti(0, 0, 30, row_height), upper_height, ui::rgba(200, 200, 200, 255), 1, false},
			{recti(30, 0, 60, row_height), 0, ui::rgba(200, 200, 200, 255), 2, false},
			{recti(0, row_height, 30, row_height * 2), lower_height, ui::rgba(120, 120, 120, 255), 3, false},
			{recti(30, row_height, 60, row_height * 2), 10, ui::rgba(120, 120, 120, 255), 4, false},
		};

		render_calendar_chart(chart, cells, style);

		std::array<int, 5> found{};

		for (auto y = 0; y < extent.cy; ++y)
		{
			for (auto x = 0; x < extent.cx; ++x)
			{
				const auto id = chart.id_at({x, y});
				if (id != chart_surface::no_id) found[id] += 1;
			}
		}

		return found;
	};

	const auto clamped_found = reachable(clamped);

	assert_equal(true, clamped_found[3] > 0, "the overlapping block is drawn", "calendar overlap");
	assert_equal(true, clamped_found[1] > 0, "the month it leans over can still be pointed at", "calendar overlap");
	assert_equal(true, clamped_found[2] > 0, "and so can an empty month beside it", "calendar overlap");
	assert_equal(true, clamped_found[4] > 0, "as can the month that did not overlap", "calendar overlap");

	const auto unclamped_found = reachable(row_height * 2 - depth_y);

	assert_equal(0, unclamped_found[1], "an unbounded block would bury the month above", "calendar overlap");
}

void register_render_tests(view_state& state, test_registry& tests)
{
	tests.add("Should unpremultiply surface alpha"s, should_unpremultiply_surface_alpha);
#if defined(COMPILE_SIMD_INTRINSIC)
	// TEST-007 - registration follows the compiled assertion body.
	tests.add("Should match SIMD software blends"s, should_match_simd_software_blends);
#endif
	tests.add("Should rasterise tiles identically to the whole surface"s,
	          should_rasterise_tiles_identically_to_the_whole_surface);
	tests.add("Should rasterise one scene to one answer on every platform"s,
	          should_rasterise_one_scene_to_one_answer_on_every_platform);
	tests.add("Should convert YUV surfaces for software rendering"s,
	          should_convert_yuv_surfaces_for_software_rendering);
	tests.add("Should tone map through a cube as a trilinear sample"s,
	          should_tone_map_through_a_cube_as_a_trilinear_sample);
	tests.add("Should area downscale packed and planar surfaces"s, should_area_downscale_packed_and_planar_surfaces);
	tests.add("Should bilinear resize packed surfaces"s, should_bilinear_resize_packed_surfaces);
	tests.add("Should estimate decode cost"s, should_estimate_decode_cost);
	tests.add("Should refuse over budget sources"s, should_refuse_over_budget_sources);
	tests.add("Should refuse an over budget raw"s, should_refuse_an_over_budget_raw);
	tests.add("Should animate alpha between values"s, should_animate_alpha_between_values);
	tests.add("Should fade at the same rate on any refresh rate"s, should_fade_at_the_same_rate_on_any_refresh_rate);
	tests.add("Should skip alpha animation when disabled"s, should_skip_alpha_animation_when_disabled);

	//
	// Sidebar charts
	//
	tests.add("Should scale month blocks logarithmically"s, should_scale_month_blocks_logarithmically);
	tests.add("Should point at the pie chart wedge that was drawn"s, should_point_at_the_pie_wedge_that_was_drawn);
	tests.add("Should reach every month block in the calendar chart"s,
	          should_reach_every_month_block_in_the_calendar);
	tests.add("Should keep an overlapped calendar month selectable"s, should_keep_an_overlapped_month_selectable);

	//
	// Colour adjustment
	//
	tests.add("Should leave colour unchanged when neutral"s, should_leave_colour_unchanged_when_neutral);
	// MEDIA-001 - saved colour adjustment must use the same BGRA channel order as preview paths.
	tests.add("Should match the preview curve to the applied curve"s,
	          should_match_the_preview_curve_to_the_applied_curve);
	tests.add("Should desaturate to grey"s, should_desaturate_to_grey);

	//
	// Surface transforms
	//
	tests.add("Should resize"s, should_resize);
	tests.add("Should rotate"s, should_rotate);
	tests.add("Should not CPU transform planar surfaces"s, should_not_cpu_transform_planar_surfaces);
	tests.add("Should rotate 133"s, should_rotate133);
	tests.add("Should render the globe"s, should_render_the_globe);
	tests.add("Should render a projected panorama"s, should_render_a_projected_panorama);
}
