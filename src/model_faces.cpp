// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The rules faces obey outside the networks: a file's face regions packed as the file states them and
// turned upright as they are read; decoded pictures handed to libmug as it reads them, and what it finds returned
// as Diffractor measures faces; and the face record a picture's faces are kept in. Owned by docs/faces.md. No
// file, database or window is touched here.

#include "pch.h"

#include "model_faces.h"

#include <numbers>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#elif defined(_M_ARM64) || defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace faces
{
	// Where a point of a picture as stored, in fractions, is shown: the EXIF meaning of each orientation, as
	// libmug::upright_map gives it for pixel centres.
	static pointd upright_point(const pointd p, const core::orientation orientation)
	{
		switch (orientation)
		{
		case core::orientation::top_right: return {1.0 - p.X, p.Y};
		case core::orientation::bottom_right: return {1.0 - p.X, 1.0 - p.Y};
		case core::orientation::bottom_left: return {p.X, 1.0 - p.Y};
		case core::orientation::left_top: return {p.Y, p.X};
		case core::orientation::right_top: return {1.0 - p.Y, p.X};
		case core::orientation::right_bottom: return {1.0 - p.Y, 1.0 - p.X};
		case core::orientation::left_bottom: return {p.Y, 1.0 - p.X};
		default: return p;
		}
	}

	// The inverse of upright_point: where a point shown upright lies on the picture as stored.
	static pointd stored_point(const pointd p, const core::orientation orientation)
	{
		switch (orientation)
		{
		case core::orientation::top_right: return {1.0 - p.X, p.Y};
		case core::orientation::bottom_right: return {1.0 - p.X, 1.0 - p.Y};
		case core::orientation::bottom_left: return {p.X, 1.0 - p.Y};
		case core::orientation::left_top: return {p.Y, p.X};
		case core::orientation::right_top: return {p.Y, 1.0 - p.X};
		case core::orientation::right_bottom: return {1.0 - p.Y, 1.0 - p.X};
		case core::orientation::left_bottom: return {1.0 - p.Y, p.X};
		default: return p;
		}
	}

	static rectd span_of(const pointd a, const pointd b)
	{
		const auto left = std::min(a.X, b.X);
		const auto top = std::min(a.Y, b.Y);
		return {left, top, std::max(a.X, b.X) - left, std::max(a.Y, b.Y) - top};
	}

	rectd to_upright(const rectd& stored, const core::orientation orientation)
	{
		return span_of(upright_point(stored.top_left(), orientation),
		               upright_point(stored.bottom_right(), orientation));
	}

	rectd to_stored(const rectd& upright, const core::orientation orientation)
	{
		return span_of(stored_point(upright.top_left(), orientation),
		               stored_point(upright.bottom_right(), orientation));
	}

	region_space space_of_regions(const sizei applied_to, const sizei stored, const core::orientation orientation)
	{
		// The shape of the picture the areas were measured on is the only evidence of the space a writer used.
		// Within a hundredth, so a resize that rounded a dimension still reads as the same shape.
		const auto same_shape = [](const sizei a, const sizei b)
		{
			if (a.cx <= 0 || a.cy <= 0 || b.cx <= 0 || b.cy <= 0) return false;
			const auto ra = static_cast<double>(a.cx) / a.cy;
			const auto rb = static_cast<double>(b.cx) / b.cy;
			return std::abs(ra - rb) <= 0.01 * std::max(ra, rb);
		};

		if (applied_to.is_empty() || stored.is_empty() || same_shape(applied_to, stored)) return region_space::stored;

		const auto shown = upright_extent(stored, orientation);
		return shown != stored && same_shape(applied_to, shown) ? region_space::upright : region_space::stale;
	}

	std::vector<named_region> upright_regions(const std::span<const stored_region> regions, const sizei applied_to,
	                                          const sizei stored, const core::orientation orientation)
	{
		const auto measured = space_of_regions(applied_to, stored, orientation);

		std::vector<named_region> result;
		result.reserve(regions.size());

		for (const auto& region : regions)
		{
			named_region r;
			r.name = region.name;

			if (region.area && measured != region_space::stale)
			{
				const auto area = measured == region_space::upright ? *region.area
				                                                    : to_upright(*region.area, orientation);
				if (area.Width > 0.0 && area.Height > 0.0) r.area = area;
			}

			result.emplace_back(std::move(r));
		}

		return result;
	}

	// One version byte, then: in version 2, the header's length and each region's fields' length - so a later release
	// can add to either and this build steps over what it does not know - the dimensions the file says its areas were
	// measured on, and each region as the file states it: its name's length and bytes, then its fields, the first its
	// area's left, top, width and height as signed 16-bit fractions of that picture, all four zero for a region with no
	// area. Version 1 kept regions already upright: each region's name and its area as unsigned 16-bit fractions.
	static constexpr uint8_t upright_regions_version = 1;
	static constexpr uint8_t stated_regions_version = 2;
	static constexpr uint8_t stated_header_len = 11; // version, header and field lengths, dimensions
	static constexpr uint8_t stated_fields_len = 8;  // area
	static constexpr size_t max_packed_regions = 255;
	// Stated areas can reach past the picture's edge, so they keep a sign and room to either side of it: -2 to 2.
	static constexpr double stated_area_scale = 16384.0;

	static void put_u16(std::string& packed, const uint16_t v)
	{
		packed.push_back(static_cast<char>(v & 0xff));
		packed.push_back(static_cast<char>(v >> 8));
	}

	static void put_u32(std::string& packed, const uint32_t v)
	{
		for (auto shift = 0; shift < 32; shift += 8) packed.push_back(static_cast<char>((v >> shift) & 0xff));
	}

	static uint16_t get_u16(const std::string_view data, const size_t pos)
	{
		return static_cast<uint16_t>(static_cast<uint8_t>(data[pos]) | static_cast<uint8_t>(data[pos + 1]) << 8);
	}

	static uint32_t get_u32(const std::string_view data, const size_t pos)
	{
		uint32_t v = 0;
		for (auto i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(data[pos + i])) << (i * 8);
		return v;
	}

	// Cut at a character's start, so a name too long to keep whole stays valid UTF-8.
	static std::string_view packable_name(const std::string_view name)
	{
		if (name.size() <= 255) return name;
		auto end = 255_z;
		while (end > 0 && (static_cast<uint8_t>(name[end]) & 0xC0) == 0x80) --end;
		return name.substr(0, end);
	}

	// An upright area held within the picture, as everything that places a region on it expects.
	static rectd within_picture(const rectd& area)
	{
		const auto left = std::clamp(area.X, 0.0, 1.0);
		const auto top = std::clamp(area.Y, 0.0, 1.0);
		return {left, top, std::clamp(area.right(), 0.0, 1.0) - left, std::clamp(area.bottom(), 0.0, 1.0) - top};
	}

	core::cached pack_regions(const std::span<const stored_region> regions, const sizei applied_to)
	{
		if (regions.empty()) return {};

		std::string packed;
		packed.push_back(static_cast<char>(stated_regions_version));
		packed.push_back(static_cast<char>(stated_header_len));
		packed.push_back(static_cast<char>(stated_fields_len));
		put_u32(packed, static_cast<uint32_t>(std::max(0, applied_to.cx)));
		put_u32(packed, static_cast<uint32_t>(std::max(0, applied_to.cy)));

		for (size_t i = 0; i < regions.size() && i < max_packed_regions; ++i)
		{
			const auto& region = regions[i];
			const auto name = packable_name(region.name);
			packed.push_back(static_cast<char>(name.size()));
			packed.append(name);

			const auto stated = [](const double v)
			{
				const auto scaled = std::isfinite(v) ? std::lround(v * stated_area_scale) : 0L;
				return static_cast<uint16_t>(static_cast<int16_t>(std::clamp(scaled, -32768L, 32767L)));
			};

			const auto has_area = region.area && region.area->Width > 0.0 && region.area->Height > 0.0;
			const auto& area = has_area ? *region.area : rectd{};
			for (const auto v : {area.X, area.Y, area.Width, area.Height}) put_u16(packed, has_area ? stated(v) : 0);
		}

		return core::cache(packed);
	}

	std::vector<named_region> unpack_regions(const core::cached packed, const sizei stored,
	                                         const core::orientation orientation)
	{
		const auto data = packed.sv();
		if (data.empty()) return {};

		const auto version = static_cast<uint8_t>(data[0]);
		if (version != upright_regions_version && version != stated_regions_version) return {};

		size_t pos = 1;
		size_t fields_len = 8;
		sizei applied_to;

		if (version == stated_regions_version)
		{
			if (data.size() < stated_header_len) return {};
			const auto header_len = static_cast<uint8_t>(data[1]);
			fields_len = static_cast<uint8_t>(data[2]);
			if (header_len < stated_header_len || fields_len < stated_fields_len || data.size() < header_len) return {};
			applied_to = {static_cast<int>(std::min<uint32_t>(get_u32(data, 3), INT32_MAX)),
			              static_cast<int>(std::min<uint32_t>(get_u32(data, 7), INT32_MAX))};
			pos = header_len;
		}

		std::vector<stored_region> stated;
		std::vector<named_region> upright;

		while (pos < data.size())
		{
			const auto name_len = static_cast<uint8_t>(data[pos++]);
			if (data.size() - pos < name_len + fields_len) return {};

			auto name = std::string(data.substr(pos, name_len));
			pos += name_len;
			const auto fields_end = pos + fields_len;

			std::array<double, 4> values{};
			for (auto& v : values)
			{
				const auto raw = get_u16(data, pos);
				v = version == stated_regions_version ? static_cast<int16_t>(raw) / stated_area_scale : raw / 65535.0;
				pos += 2;
			}

			pos = fields_end;

			const rectd area{values[0], values[1], values[2], values[3]};
			const auto has_area = area.Width > 0.0 && area.Height > 0.0;

			if (version == stated_regions_version)
			{
				stated.push_back({std::move(name), has_area ? std::optional(area) : std::nullopt});
			}
			else
			{
				upright.push_back({std::move(name), has_area ? area : rectd{}});
			}
		}

		if (version == upright_regions_version) return upright;

		auto result = upright_regions(stated, applied_to, stored, orientation);
		for (auto& r : result)
		{
			if (!r.has_area()) continue;
			r.area = within_picture(r.area);
			if (!r.has_area()) r.area = {};
		}
		return result;
	}
	const tuning& defaults()
	{
		static const tuning t;
		return t;
	}

	sizei upright_extent(const sizei extent, const core::orientation orientation)
	{
		return core::flips_xy(orientation) ? sizei{extent.cy, extent.cx} : extent;
	}

	namespace
	{
		// core::orientation numbers its turns as EXIF does, as libmug's does; none is upright.
		libmug::orientation to_libmug(const core::orientation orientation)
		{
			const auto value = static_cast<int>(orientation);
			return value >= 1 && value <= 8 ? static_cast<libmug::orientation>(value) : libmug::orientation::top_left;
		}

		bool is_limited(const core::color_space cs)
		{
			return cs == core::color_space::rec601_limited || cs == core::color_space::rec709_limited ||
			       cs == core::color_space::rec2020_limited;
		}

		// A decode as libmug reads it, without a copy: packed 32-bit as its blue, green and red, NV12 as its two
		// planes, each with the orientation it is shown in. None for any other format.
		std::optional<libmug::image_view> view_of(const core::const_surface_ptr& surface)
		{
			if (!core::is_valid(surface)) return {};

			const auto extent = surface->dimensions();
			libmug::image_view view;
			view.width = extent.cx;
			view.height = extent.cy;
			view.pixels = surface->pixels();
			view.stride = static_cast<std::ptrdiff_t>(surface->stride());
			view.orientation = to_libmug(surface->orientation());

			if (core::is_packed(surface->format()))
			{
				view.format = libmug::pixel_format::bgrx8;
				return view;
			}

			// The chroma plane follows the luma plane, at its stride: NV12 surfaces are even in both dimensions.
			if (surface->format() == core::texture_format::NV12 && extent.cx % 2 == 0 && extent.cy % 2 == 0)
			{
				view.format = libmug::pixel_format::nv12;
				view.chroma = surface->pixels() + surface->stride() * extent.cy;
				view.chroma_stride = view.stride;
				view.limited_range = is_limited(surface->color_space());
				return view;
			}

			return {};
		}

		libmug::detect_options detect_options_of(const tuning& t)
		{
			libmug::detect_options options;
			options.confidence = t.detector_confidence;
			options.edge = t.detector_edge;
			options.min_face_edge = t.min_face_edge_fraction;
			return options;
		}

		face_set faces_of(const libmug::face_set& found)
		{
			face_set result;
			result.extent = {found.shown.width, found.shown.height};
			result.faces.reserve(found.faces.size());

			for (const auto& f : found.faces)
			{
				face converted;
				converted.bounds = {f.bounds.x, f.bounds.y, f.bounds.width, f.bounds.height};
				for (auto i = 0; i < landmark_count; ++i) converted.marks[i] = {f.marks[i].x, f.marks[i].y};
				converted.confidence = f.confidence;
				result.faces.push_back(converted);
			}

			return result;
		}
	}

	face_set find_faces(libmug::face_detector& detector, const core::const_surface_ptr& surface, const tuning& t)
	{
		const auto view = view_of(surface);
		if (!view) return {};
		return faces_of(detector.detect(*view, detect_options_of(t)));
	}

	bool embed_face(libmug::face_embedder& embedder, const core::const_surface_ptr& decoded, const landmarks& marks,
	                face_vector& result, const tuning& t)
	{
		// libmug reads NV12's colour as BT.601's, which the crop depends on.
		const auto view = view_of(decoded);
		if (!view ||
		    (view->format == libmug::pixel_format::nv12 && decoded->color_space() != core::color_space::rec601_full &&
		     decoded->color_space() != core::color_space::rec601_limited))
			return false;

		libmug::landmarks points{};
		for (auto i = 0; i < landmark_count; ++i) points[i] = {marks[i].X, marks[i].Y};
		return embedder.embed(*view, points, t.max_outside, result);
	}

	uint32_t model_id()
	{
		// What a face hash depends on beyond libmug: the bounded, fitted decode its picture is read from.
		// Moved whenever that changes, so faces measured the old way are compared with nothing until they are
		// read again. Never zero, which a record that has none reads as.
		constexpr uint32_t pipeline_version = 1;
		const auto id = libmug::model_id() ^ (pipeline_version * 0x9E3779B1u);
		return id == 0 ? 1u : id;
	}

	core::surface_ptr cut_tile(const core::const_surface_ptr& decoded, const rectd& upright_box, const int edge)
	{
		if (!core::is_valid(decoded) || !core::is_packed(decoded->format()) || edge < 1) return {};

		const auto extent = decoded->dimensions();
		const auto shown = upright_extent(extent, decoded->orientation());
		const auto from_upright = libmug::upright_map(extent.cx, extent.cy, to_libmug(decoded->orientation())).invert();
		if (!from_upright || shown.cx < 1 || shown.cy < 1) return {};

		// The square around the face, in upright pixels, a little above its middle to take in the hair. It is kept
		// inside the picture - no wider than its shorter side, and moved in from an edge - so a face near the edge sits
		// off the tile's middle rather than beside a smear of the picture's edge pixels.
		auto side = static_cast<double>(std::min(shown.cx, shown.cy));
		auto cx = shown.cx / 2.0;
		auto cy = shown.cy / 2.0;

		if (upright_box.Width > 0.0 && upright_box.Height > 0.0)
		{
			side = std::max(upright_box.Width * shown.cx, upright_box.Height * shown.cy) * 1.6;
			side = std::clamp(side, 2.0, static_cast<double>(std::min(shown.cx, shown.cy)));
			cx = (upright_box.X + upright_box.Width / 2.0) * shown.cx;
			cy = (upright_box.Y + upright_box.Height / 2.0) * shown.cy - side * 0.05;
			cx = std::clamp(cx, side / 2.0, std::max(side / 2.0, shown.cx - side / 2.0));
			cy = std::clamp(cy, side / 2.0, std::max(side / 2.0, shown.cy - side / 2.0));
		}

		auto result = std::make_shared<core::surface>();
		result->alloc(edge, edge, core::texture_format::RGB);

		const auto last_x = extent.cx - 1;
		const auto last_y = extent.cy - 1;
		const auto step = side / edge;

		for (auto y = 0; y < edge; ++y)
		{
			auto* out = result->pixels_line(y);

			for (auto x = 0; x < edge; ++x, out += 4)
			{
				// Pixel centres at whole coordinates, the edge pixels repeated past the picture.
				const auto p = from_upright->apply(
					{cx - side / 2.0 + (x + 0.5) * step - 0.5, cy - side / 2.0 + (y + 0.5) * step - 0.5});
				const auto px = std::clamp(p.x, 0.0, static_cast<double>(last_x));
				const auto py = std::clamp(p.y, 0.0, static_cast<double>(last_y));
				const auto x0 = static_cast<int>(px);
				const auto y0 = static_cast<int>(py);
				const auto x1 = std::min(x0 + 1, last_x);
				const auto y1 = std::min(y0 + 1, last_y);
				const auto fx = px - x0;
				const auto fy = py - y0;
				const auto* const a = decoded->pixels_line(y0) + static_cast<size_t>(x0) * 4;
				const auto* const b = decoded->pixels_line(y0) + static_cast<size_t>(x1) * 4;
				const auto* const c = decoded->pixels_line(y1) + static_cast<size_t>(x0) * 4;
				const auto* const d = decoded->pixels_line(y1) + static_cast<size_t>(x1) * 4;

				for (auto k = 0; k < 3; ++k)
				{
					const auto top = a[k] + (b[k] - a[k]) * fx;
					const auto bottom = c[k] + (d[k] - c[k]) * fx;
					out[k] = static_cast<uint8_t>(std::clamp(std::lround(top + (bottom - top) * fy), 0L, 255L));
				}

				out[3] = 255;
			}
		}

		return result;
	}

	face_quality measure_quality(const landmarks& marks)
	{
		const auto& left = marks[0];
		const auto& right = marks[1];
		const auto& nose = marks[2];
		const auto ex = right.X - left.X;
		const auto ey = right.Y - left.Y;
		const auto distance = std::hypot(ex, ey);
		if (!(distance > 0.0)) return {};

		// The nose along the line between the eyes, in eye distances from their midpoint: a face turned to one
		// side brings it over one eye.
		const auto mx = (left.X + right.X) / 2.0;
		const auto my = (left.Y + right.Y) / 2.0;
		const auto along = ((nose.X - mx) * ex + (nose.Y - my) * ey) / (distance * distance);
		const auto even = std::clamp(1.0 - std::abs(along) * 2.0, 0.0, 1.0);

		// Eyes past an eighth of a turn from level are a face on its side, or no face at all.
		const auto tilt = std::abs(std::atan2(ey, ex));
		const auto level = std::clamp(1.0 - tilt / (std::numbers::pi / 4.0), 0.0, 1.0);

		return {distance, even * level};
	}

	bool is_good(const face_quality& quality, const int confidence, const tuning& t)
	{
		return quality.eye_distance >= t.good_eye_distance && quality.frontal >= t.good_frontal &&
		       confidence >= t.good_confidence;
	}

	face_hash quantize(const face_vector& v)
	{
		face_hash result;
		auto largest = 0.0f;
		for (const auto value : v) largest = std::max(largest, std::abs(value));
		if (!(largest > 0.0f) || !std::isfinite(largest)) return result;

		result.scale = largest / 127.0f;
		for (auto i = 0; i < vector_size; ++i)
		{
			result.values[i] = static_cast<int8_t>(std::clamp(std::lround(v[i] / result.scale), -127L, 127L));
		}
		return result;
	}

#if defined(_M_X64)
	// Each byte widened to 16 bits by its sign as it is loaded, sixteen at a time: the same exact sums as the SSE2 code
	// below in a third of the instructions, so either gives the same answer to the bit.
	static int32_t dot_values_avx2(const int8_t* a, const int8_t* b)
	{
		auto sum = _mm256_setzero_si256();
		for (auto i = 0; i < vector_size; i += 16)
		{
			const auto va = _mm256_cvtepi8_epi16(_mm_loadu_si128(std::bit_cast<const __m128i*>(a + i)));
			const auto vb = _mm256_cvtepi8_epi16(_mm_loadu_si128(std::bit_cast<const __m128i*>(b + i)));
			sum = _mm256_add_epi32(sum, _mm256_madd_epi16(va, vb));
		}
		auto half = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
		half = _mm_add_epi32(half, _mm_shuffle_epi32(half, 0x4e));
		half = _mm_add_epi32(half, _mm_shuffle_epi32(half, 0xb1));
		return _mm_cvtsi128_si32(half);
	}

	static const bool dot_with_avx2 = core::has_avx2();
#endif

	// Every comparison of faces comes down to this, millions of times a pass: written for the processor, since a plain
	// loop is not vectorized where the build favours size, and runs eight times slower.
	int32_t dot_values(const int8_t* a, const int8_t* b)
	{
		static_assert(vector_size % 16 == 0);
#if defined(_M_X64)
		if (dot_with_avx2) return dot_values_avx2(a, b);
#endif
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
		// Bytes widened to 16 bits by their sign, multiplied and summed in pairs.
		const auto zero = _mm_setzero_si128();
		auto sum = _mm_setzero_si128();
		for (auto i = 0; i < vector_size; i += 16)
		{
			const auto va = _mm_loadu_si128(std::bit_cast<const __m128i*>(a + i));
			const auto vb = _mm_loadu_si128(std::bit_cast<const __m128i*>(b + i));
			const auto sa = _mm_cmpgt_epi8(zero, va);
			const auto sb = _mm_cmpgt_epi8(zero, vb);
			sum = _mm_add_epi32(sum, _mm_madd_epi16(_mm_unpacklo_epi8(va, sa), _mm_unpacklo_epi8(vb, sb)));
			sum = _mm_add_epi32(sum, _mm_madd_epi16(_mm_unpackhi_epi8(va, sa), _mm_unpackhi_epi8(vb, sb)));
		}
		sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0x4e));
		sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0xb1));
		return _mm_cvtsi128_si32(sum);
#elif defined(_M_ARM64) || defined(__aarch64__)
		auto sum = vdupq_n_s32(0);
		for (auto i = 0; i < vector_size; i += 16)
		{
			const auto va = vld1q_s8(a + i);
			const auto vb = vld1q_s8(b + i);
			sum = vpadalq_s16(sum, vmull_s8(vget_low_s8(va), vget_low_s8(vb)));
			sum = vpadalq_s16(sum, vmull_s8(vget_high_s8(va), vget_high_s8(vb)));
		}
		return vaddvq_s32(sum);
#else
		int32_t dot = 0;
		for (auto i = 0; i < vector_size; ++i) dot += a[i] * b[i];
		return dot;
#endif
	}

	float similarity(const face_hash& a, const face_hash& b)
	{
		return static_cast<float>(dot_values(a.values.data(), b.values.data())) * a.scale * b.scale;
	}

	float similarity(const face_vector& a, const face_vector& b)
	{
		auto dot = 0.0f;
		for (auto i = 0; i < vector_size; ++i) dot += a[i] * b[i];
		return dot;
	}

	// ---- The face record -----------------------------------------------------------------------------

	// The record's header, then each face at a stride the header states, then the dismissals, then in a video the parts
	// each face's person was seen in: a reader takes the fields it knows from the front of each face and steps over the
	// rest, and leaves a section it does not know unread, so a later release can add a field to a face or a section
	// without the collection being read again.
	static constexpr uint8_t record_format = 1;
	static constexpr uint8_t record_header_len =
		24; // format, sizes, count, model, stamp, extent, status, detection, flags
	static constexpr uint8_t record_face_stride = 170; // box, landmarks, measures, flags, scale, values, time
	// A face before it kept a time: what a reader needs of one.
	static constexpr uint8_t record_face_stride_least = 166;
	// The header's flags.
	static constexpr uint16_t record_from_video = 1;
	// Positions as fractions of the upright picture in 16 bits, from -2 to 2, so a box past the edge is kept.
	static constexpr double position_scale = 16384.0;

	// Whether a face is good, judged from the measures its record keeps rather than kept as a judgment: a face read
	// back is judged by the floors of the build reading it, so a correction to them needs no picture read again, and
	// a face just measured is judged on the same rounded measures it is kept with.
	static void judge_good(found_face& f, const tuning& t)
	{
		const face_quality measured{static_cast<double>(f.eye_distance), f.frontal / 255.0};
		if (is_good(measured, f.confidence, t)) f.flags |= face_flag::good;
		else f.flags &= static_cast<uint8_t>(~face_flag::good);
	}

	namespace
	{
		struct record_writer
		{
			std::vector<uint8_t>& out;

			template <typename T> void put(const T value)
			{
				const auto* const p = std::bit_cast<const uint8_t*>(&value);
				out.insert(out.end(), p, p + sizeof(T));
			}

			void position(const double v)
			{
				put(static_cast<int16_t>(std::clamp(std::lround(v * position_scale), -32768L, 32767L)));
			}
		};

		struct record_reader
		{
			std::span<const uint8_t> in;
			size_t pos = 0;

			bool has(const size_t n) const { return pos <= in.size() && in.size() - pos >= n; }

			template <typename T> T get()
			{
				T value{};
				std::memcpy(&value, in.data() + pos, sizeof(T));
				pos += sizeof(T);
				return value;
			}

			double position() { return get<int16_t>() / position_scale; }
		};
	}

	std::vector<uint8_t> pack_record(const face_record& record)
	{
		std::vector<uint8_t> out;
		const auto count = std::min<size_t>(record.faces.size(), UINT8_MAX);
		out.reserve(record_header_len + count * record_face_stride + 8);
		record_writer w{out};

		w.put(record_format);
		w.put(record_header_len);
		w.put(record_face_stride);
		w.put(static_cast<uint8_t>(count));
		w.put(record.model);
		w.put(record.stamp.to_int64());
		w.put(static_cast<uint16_t>(std::clamp(record.extent.cx, 0, 65535)));
		w.put(static_cast<uint16_t>(std::clamp(record.extent.cy, 0, 65535)));
		w.put(static_cast<uint8_t>(record.status));
		w.put(record.detection_revision);
		w.put(record.video ? record_from_video : uint16_t{0});

		size_t dismissals = 0;
		size_t spans = 0;

		for (size_t i = 0; i < count; ++i)
		{
			const auto& f = record.faces[i];
			w.position(f.box.X);
			w.position(f.box.Y);
			w.position(f.box.Width);
			w.position(f.box.Height);
			for (const auto& p : f.marks)
			{
				w.position(p.X);
				w.position(p.Y);
			}
			w.put(f.confidence);
			w.put(f.frontal);
			w.put(f.eye_distance);
			w.put(f.flags);
			w.put(uint8_t{0});
			w.put(f.hash.scale);
			for (const auto v : f.hash.values) w.put(v);
			w.put(f.time_ms);
			dismissals += std::min<size_t>(f.dismissed.size(), UINT8_MAX);
			spans += f.seen.size();
		}

		// Each dismissal: the face's position, then the name's length and its bytes.
		w.put(static_cast<uint16_t>(std::min<size_t>(dismissals, UINT16_MAX)));
		size_t written = 0;

		for (size_t i = 0; i < count; ++i)
		{
			const auto& dismissed = record.faces[i].dismissed;
			for (size_t k = 0; k < dismissed.size() && k < UINT8_MAX && written < UINT16_MAX; ++k, ++written)
			{
				const auto name = std::string_view(dismissed[k]).substr(0, 255);
				w.put(static_cast<uint8_t>(i));
				w.put(static_cast<uint8_t>(name.size()));
				out.insert(out.end(), name.begin(), name.end());
			}
		}

		// Each part a video's face was seen in: the face's position, then where the part starts and ends. Only a video
		// has any, so a picture's record ends with its dismissals.
		if (spans > 0)
		{
			const auto kept = std::min<size_t>(spans, UINT16_MAX);
			w.put(static_cast<uint16_t>(kept));
			size_t put = 0;

			for (size_t i = 0; i < count; ++i)
			{
				for (const auto& s : record.faces[i].seen)
				{
					if (put++ == kept) break;
					w.put(static_cast<uint8_t>(i));
					w.put(s.start);
					w.put(s.end);
				}
			}
		}

		return out;
	}

	std::optional<face_record> unpack_record(const std::span<const uint8_t> data)
	{
		record_reader r{data};
		if (!r.has(4)) return {};

		const auto format = r.get<uint8_t>();
		const auto header_len = r.get<uint8_t>();
		const auto stride = r.get<uint8_t>();
		const auto count = r.get<uint8_t>();

		if (format != record_format || header_len < record_header_len || stride < record_face_stride_least) return {};
		if (!r.has(header_len - 4_z + static_cast<size_t>(count) * stride + 2)) return {};

		face_record record;
		record.model = r.get<uint32_t>();
		record.stamp = core::date_t(r.get<int64_t>());
		const auto w = r.get<uint16_t>();
		const auto h = r.get<uint16_t>();
		record.extent = {w, h};
		record.status = static_cast<record_status>(r.get<uint8_t>());
		record.detection_revision = r.get<uint8_t>();
		record.video = (r.get<uint16_t>() & record_from_video) != 0;
		r.pos = header_len;

		record.faces.resize(count);

		for (auto& f : record.faces)
		{
			const auto start = r.pos;
			const auto x = r.position();
			const auto y = r.position();
			const auto bw = r.position();
			const auto bh = r.position();
			f.box = {x, y, bw, bh};
			for (auto& p : f.marks)
			{
				p.X = r.position();
				p.Y = r.position();
			}
			f.confidence = r.get<uint8_t>();
			f.frontal = r.get<uint8_t>();
			f.eye_distance = r.get<uint16_t>();
			f.flags = r.get<uint8_t>();
			judge_good(f, defaults());
			r.get<uint8_t>();
			f.hash.scale = r.get<float>();
			for (auto& v : f.hash.values) v = r.get<int8_t>();
			if (!std::isfinite(f.hash.scale) || f.hash.scale < 0.0f) f.hash = {};
			if (stride >= record_face_stride) f.time_ms = r.get<uint32_t>();
			r.pos = start + stride;
		}

		const auto dismissals = r.get<uint16_t>();

		for (auto i = 0; i < dismissals; ++i)
		{
			if (!r.has(2)) return {};
			const auto face = r.get<uint8_t>();
			const auto len = r.get<uint8_t>();
			if (!r.has(len) || face >= record.faces.size()) return {};
			record.faces[face].dismissed.emplace_back(std::bit_cast<const char*>(data.data() + r.pos), len);
			r.pos += len;
		}

		if (r.has(2))
		{
			const auto spans = r.get<uint16_t>();
			constexpr size_t span_len = 9;
			if (!r.has(spans * span_len)) return {};

			for (auto i = 0; i < spans; ++i)
			{
				const auto face = r.get<uint8_t>();
				const auto from = r.get<uint32_t>();
				const auto to = r.get<uint32_t>();
				if (face >= record.faces.size() || to < from) return {};
				record.faces[face].seen.push_back({from, to});
			}
		}

		return record;
	}

	face_record turn_record(face_record record, const core::orientation from, const core::orientation to)
	{
		const auto move = [from, to](const pointd p)
		{
			return upright_point(stored_point(p, from), to);
		};

		const auto stored = upright_extent(record.extent, from);
		record.extent = upright_extent(stored, to);

		for (auto& f : record.faces)
		{
			f.box = span_of(move(f.box.top_left()), move(f.box.bottom_right()));
			for (auto& p : f.marks) p = move(p);
		}

		return record;
	}

	// ---- Reading a picture ---------------------------------------------------------------------------

	void carry_marks(const face_record& from, face_record& to)
	{
		std::vector<named_region> places;
		places.reserve(from.faces.size());
		for (const auto& f : from.faces) places.push_back({{}, f.box});

		const auto matched = match_regions(places, to.faces);

		for (size_t i = 0; i < matched.size(); ++i)
		{
			if (matched[i] < 0) continue;
			const auto& was = from.faces[i];
			auto& face = to.faces[static_cast<size_t>(matched[i])];

			if (was.is_hidden()) face.flags |= face_flag::hidden;

			for (const auto& person : was.dismissed)
			{
				if (std::ranges::none_of(face.dismissed,
				                         [&person](const std::string& name) { return core::icmp(name, person) == 0; }))
					face.dismissed.push_back(person);
			}
		}
	}

	double region_overlap(const rectd& face_box, const rectd& region)
	{
		const auto width = std::min(face_box.right(), region.right()) - std::max(face_box.left(), region.left());
		const auto height = std::min(face_box.bottom(), region.bottom()) - std::max(face_box.top(), region.top());
		if (width <= 0.0 || height <= 0.0) return 0.0;

		const auto intersection = width * height;
		const auto face_area = face_box.Width * face_box.Height;
		const auto region_area = region.Width * region.Height;
		if (face_area <= 0.0 || region_area <= 0.0) return 0.0;

		// One face when they overlap by enough of their union, or when the region holds most of the face and is
		// not so much larger that it could be a picture of a crowd: people draw a region around the head, where
		// the detector boxes the face within it.
		const auto union_share = intersection / (face_area + region_area - intersection);
		const auto held = intersection / face_area;
		if (union_share >= 0.3) return union_share;
		if (held >= 0.6 && region_area <= face_area * 6.0) return union_share + 0.01;
		return 0.0;
	}

	std::vector<int> match_regions(const std::span<const named_region> regions, const std::span<const found_face> faces)
	{
		struct candidate
		{
			double overlap;
			int region;
			int face;
		};

		std::vector<candidate> candidates;

		for (auto r = 0; r < static_cast<int>(regions.size()); ++r)
		{
			if (!regions[r].has_area()) continue;
			for (auto f = 0; f < static_cast<int>(faces.size()); ++f)
			{
				if (const auto overlap = region_overlap(faces[f].box, regions[r].area); overlap > 0.0)
					candidates.push_back({overlap, r, f});
			}
		}

		std::ranges::stable_sort(candidates,
		                         [](const candidate& a, const candidate& b) { return a.overlap > b.overlap; });

		std::vector result(regions.size(), -1);
		std::vector taken(faces.size(), false);

		for (const auto& c : candidates)
		{
			if (result[c.region] >= 0 || taken[c.face]) continue;
			result[c.region] = c.face;
			taken[c.face] = true;
		}

		return result;
	}

	namespace
	{
		// One face measured and embedded, its box and landmarks as fractions of the extent.
		found_face measure_face(libmug::face_embedder& embedder, const core::const_surface_ptr& decoded, const face& f,
		                        const sizei extent, const tuning& t)
		{
			found_face result;
			const auto sx = 1.0 / std::max(1, extent.cx);
			const auto sy = 1.0 / std::max(1, extent.cy);
			result.box = {f.bounds.X * sx, f.bounds.Y * sy, f.bounds.Width * sx, f.bounds.Height * sy};
			for (auto i = 0; i < landmark_count; ++i) result.marks[i] = {f.marks[i].X * sx, f.marks[i].Y * sy};

			const auto quality = measure_quality(f.marks);
			result.confidence = static_cast<uint8_t>(std::clamp(f.confidence, 0, 100));
			result.frontal = static_cast<uint8_t>(std::clamp(std::lround(quality.frontal * 255.0), 0L, 255L));
			result.eye_distance = static_cast<uint16_t>(std::clamp(std::lround(quality.eye_distance), 0L, 65535L));
			judge_good(result, t);

			face_vector v{};
			if (embed_face(embedder, decoded, f.marks, v, t)) result.hash = quantize(v);
			return result;
		}

		// The stored pixels under an upright rectangle of fractions of a decode, in the decode's own orientation.
		// False where they are too few to find a face in.
		bool stored_pixels(const core::const_surface_ptr& decoded, const rectd& upright, recti& stored_px)
		{
			const auto stored = to_stored(upright, decoded->orientation());
			const auto dims = decoded->dimensions();
			stored_px = recti{std::clamp(static_cast<int>(std::floor(stored.X * dims.cx)), 0, dims.cx),
			                  std::clamp(static_cast<int>(std::floor(stored.Y * dims.cy)), 0, dims.cy),
			                  std::clamp(static_cast<int>(std::ceil(stored.right() * dims.cx)), 0, dims.cx),
			                  std::clamp(static_cast<int>(std::ceil(stored.bottom() * dims.cy)), 0, dims.cy)};
			return stored_px.width() >= 16 && stored_px.height() >= 16;
		}

		std::vector<face> detect_mirrored(libmug::face_detector& detector, libmug::image_view picture,
		                                  const libmug::detect_options& options)
		{
			using enum libmug::orientation;
			constexpr std::array reflected{top_right, top_left, bottom_left, bottom_right,
			                               right_top, left_top, left_bottom, right_bottom};
			picture.orientation = reflected[static_cast<int>(picture.orientation) - 1];
			auto found = detector.detect(picture, options);
			for (auto& f : found.faces)
			{
				f.bounds.x = found.shown.width - (f.bounds.x + f.bounds.width);
				for (auto& p : f.marks) p.x = found.shown.width - 1.0 - p.x;
				std::swap(f.marks[0], f.marks[1]);
				std::swap(f.marks[3], f.marks[4]);
			}
			libmug::order_faces(found.faces);
			return faces_of(found).faces;
		}
	}

	face_record read_record(libmug::face_detector& detector, libmug::face_embedder& embedder,
	                        const core::const_surface_ptr& decoded, const std::span<const named_region> regions,
	                        const tuning& t)
	{
		face_record record;
		record.model = model_id();

		const auto picture = core::is_valid(decoded) && core::is_packed(decoded->format()) ? view_of(decoded)
		                                                                                   : std::nullopt;
		if (!picture)
		{
			record.status = record_status::undecodable;
			return record;
		}

		const auto options = detect_options_of(t);
		auto probe = options;
		// Weak small candidates can justify a closer look, but cannot themselves become reported faces.
		probe.confidence = std::min(options.confidence, 55);
		probe.min_face_edge = std::min(options.min_face_edge, 0.02);
		auto found = faces_of(detector.detect(*picture, probe));
		record.extent = found.extent;
		const auto short_edge = std::min(found.extent.cx, found.extent.cy);
		const auto small = short_edge * 0.08;
		const auto face_edge = [](const face& f)
		{
			return std::min(f.bounds.Width, f.bounds.Height);
		};
		const auto distant = std::max(found.extent.cx, found.extent.cy) > options.edge &&
		                     std::ranges::any_of(found.faces, [&](const face& f) { return face_edge(f) <= small; });
		std::erase_if(found.faces, [&](const face& f)
		{ return f.confidence < options.confidence || face_edge(f) < short_edge * options.min_face_edge; });
		const auto close = std::ranges::any_of(found.faces,
		                                       [&](const face& f) { return face_edge(f) >= short_edge * 0.15; });

		// The largest, where a crowd holds more than a picture keeps, in the stable order still.
		if (found.faces.size() > static_cast<size_t>(t.max_faces))
		{
			std::vector<size_t> order(found.faces.size());
			std::iota(order.begin(), order.end(), 0_z);
			std::ranges::stable_sort(order, [&found](const size_t a, const size_t b)
			{
				return found.faces[a].bounds.Width * found.faces[a].bounds.Height >
				       found.faces[b].bounds.Width * found.faces[b].bounds.Height;
			});
			order.resize(static_cast<size_t>(t.max_faces));
			std::ranges::sort(order);
			std::vector<face> kept;
			kept.reserve(order.size());
			for (const auto i : order) kept.push_back(found.faces[i]);
			found.faces = std::move(kept);
		}

		for (const auto& f : found.faces) record.faces.push_back(measure_face(embedder, decoded, f, found.extent, t));

		// A named region the detector missed is looked for again within its own area, enlarged, where a
		// small or turned face is larger against the plane the detector reads.
		const auto matched = match_regions(regions, record.faces);

		for (size_t r = 0; r < regions.size() && record.faces.size() < static_cast<size_t>(t.max_faces); ++r)
		{
			if (matched[r] >= 0 || !regions[r].has_area() || regions[r].name.empty()) continue;

			const auto& area = regions[r].area;
			const auto grow_x = area.Width * 0.5;
			const auto grow_y = area.Height * 0.5;
			const rectd wider{std::max(0.0, area.X - grow_x), std::max(0.0, area.Y - grow_y),
			                  std::min(1.0, area.right() + grow_x) - std::max(0.0, area.X - grow_x),
			                  std::min(1.0, area.bottom() + grow_y) - std::max(0.0, area.Y - grow_y)};

			recti stored_px;
			if (!stored_pixels(decoded, wider, stored_px)) continue;

			// libmug reads the part where it lies, turned as the whole picture is.
			auto within = faces_of(
				detector.detect(picture->part(stored_px.left, stored_px.top, stored_px.width(), stored_px.height()),
			                    detect_options_of(t)));
			if (within.faces.empty()) continue;

			// The most confident face of the part that is not one already found - a neighbour the enlarged area reaches
			// - measured against the whole picture.
			const auto scale_x = wider.Width * found.extent.cx / std::max(1, within.extent.cx);
			const auto scale_y = wider.Height * found.extent.cy / std::max(1, within.extent.cy);
			const auto to_whole = [&](const pointd p)
			{
				return pointd{wider.X * found.extent.cx + p.X * scale_x, wider.Y * found.extent.cy + p.Y * scale_y};
			};

			std::optional<face> whole;
			for (const auto& candidate : within.faces)
			{
				if (whole && whole->confidence >= candidate.confidence) continue;
				face f;
				f.confidence = candidate.confidence;
				const auto top_left = to_whole(candidate.bounds.top_left());
				f.bounds = {top_left.X, top_left.Y, candidate.bounds.Width * scale_x,
				            candidate.bounds.Height * scale_y};
				const rectd box{f.bounds.X / found.extent.cx, f.bounds.Y / found.extent.cy,
				                f.bounds.Width / found.extent.cx, f.bounds.Height / found.extent.cy};
				if (std::ranges::any_of(record.faces, [&](const found_face& prior)
				{ return region_overlap(box, prior.box) > 0 || region_overlap(prior.box, box) > 0; }))
					continue;
				for (auto i = 0; i < landmark_count; ++i) f.marks[i] = to_whole(candidate.marks[i]);
				whole = f;
			}
			if (!whole) continue;

			auto measured = measure_face(embedder, decoded, *whole, found.extent, t);
			if (region_overlap(measured.box, area) <= 0.0) continue;
			measured.flags |= face_flag::in_region;
			record.faces.push_back(std::move(measured));
		}

		if (options.edge > 0 && (distant || close) && record.faces.size() < static_cast<size_t>(t.max_faces))
		{
			// More pixels for distant faces; a smaller mirrored plane for close-ups and difficult profiles.
			auto retry = options;
			retry.edge = distant ? core::mul_div(options.edge, 8, 5) : core::mul_div(options.edge, 3, 4);
			if (distant) retry.min_face_edge = probe.min_face_edge;
			for (const auto& f : detect_mirrored(detector, *picture, retry))
			{
				if (record.faces.size() == static_cast<size_t>(t.max_faces)) break;
				if (distant && face_edge(f) > small) continue;
				const auto box = rectd{f.bounds.X / found.extent.cx, f.bounds.Y / found.extent.cy,
				                       f.bounds.Width / found.extent.cx, f.bounds.Height / found.extent.cy};
				if (std::ranges::any_of(record.faces, [&](const found_face& prior)
				{ return region_overlap(box, prior.box) > 0 || region_overlap(prior.box, box) > 0; }))
					continue;
				// Do not replace an earlier detection: its position and exact face-book signature must survive.
				record.faces.push_back(measure_face(embedder, decoded, f, found.extent, t));
			}
		}

		return record;
	}

	// ---- Reading a video -----------------------------------------------------------------------------

	namespace
	{
		// How clearly a face shows who it is: a good face first, then the larger, the more square-on and the more
		// confidently found.
		double clarity(const found_face& f)
		{
			return (f.is_good() ? 1e6 : 0.0) + f.eye_distance * (0.5 + f.frontal / 255.0) * (f.confidence / 100.0);
		}

		// How much of their union two boxes share.
		double shared_area(const rectd& a, const rectd& b)
		{
			const auto width = std::min(a.right(), b.right()) - std::max(a.left(), b.left());
			const auto height = std::min(a.bottom(), b.bottom()) - std::max(a.top(), b.top());
			if (width <= 0.0 || height <= 0.0) return 0.0;
			const auto both = width * height;
			return both / (a.Width * a.Height + b.Width * b.Height - both);
		}

		uint32_t to_ms(const double seconds)
		{
			return static_cast<uint32_t>(
				std::clamp(std::llround(seconds * 1000.0), 0ll, static_cast<long long>(UINT32_MAX)));
		}
	}

	face_record merge_video_frames(std::vector<video_frame> frames, const double length, const tuning& t)
	{
		face_record result;
		result.model = model_id();
		result.video = true;

		std::erase_if(frames, [](const video_frame& f) { return f.found.status != record_status::read; });
		if (frames.empty())
		{
			result.status = record_status::undecodable;
			return result;
		}

		std::ranges::stable_sort(frames, {}, &video_frame::time);
		result.extent = frames.front().found.extent;

		struct sighting
		{
			size_t frame = 0;
			size_t face = 0;
		};

		struct person
		{
			std::vector<sighting> seen;
			sighting clearest;
			double clarity = -1.0;
		};

		const auto face_of = [&frames](const sighting s) -> const found_face&
		{
			return frames[s.frame].found.faces[s.face];
		};

		std::vector<person> people;

		const auto add = [&](person& p, const sighting s)
		{
			p.seen.push_back(s);
			if (const auto c = clarity(face_of(s)); c > p.clarity)
			{
				p.clarity = c;
				p.clearest = s;
			}
		};

		// Whom a face is compared with: the person's clearest face, their first and the last few they were seen with,
		// so someone turning through a video stays one person, and a long video costs no more to compare than a short.
		const auto likeness = [&](const person& p, const found_face& f)
		{
			auto best = -1.0f;
			const auto compare = [&](const sighting s)
			{
				best = std::max(best, similarity(face_of(s).hash, f.hash));
			};
			compare(p.clearest);
			compare(p.seen.front());
			const auto recent = std::min<size_t>(p.seen.size(), 4);
			for (auto i = p.seen.size() - recent; i < p.seen.size(); ++i) compare(p.seen[i]);
			return best;
		};

		for (size_t fi = 0; fi < frames.size(); ++fi)
		{
			const auto& faces = frames[fi].found.faces;

			struct pairing
			{
				size_t face;
				size_t person;
				float likeness;
			};

			std::vector<pairing> pairings;

			for (size_t k = 0; k < faces.size(); ++k)
			{
				const auto& f = faces[k];
				if (f.hash.is_empty()) continue;

				for (size_t p = 0; p < people.size(); ++p)
				{
					// In the place they stood in the frame read before, someone is themselves at a lower likeness:
					// turned away, or blurred as they move.
					const auto last = people[p].seen.back();
					const auto nearby = last.frame + 1 == fi && shared_area(face_of(last).box, f.box) >= 0.3;
					const auto alike = likeness(people[p], f);
					if (alike >= (nearby ? t.video_nearby : t.video_same)) pairings.push_back({k, p, alike});
				}
			}

			// The likeliest first. Two faces in one frame are two people, so each person is seen once a frame.
			std::ranges::stable_sort(pairings,
			                         [](const pairing& a, const pairing& b) { return a.likeness > b.likeness; });
			std::vector placed(faces.size(), false);
			std::vector taken(people.size(), false);

			for (const auto& p : pairings)
			{
				if (placed[p.face] || taken[p.person]) continue;
				placed[p.face] = taken[p.person] = true;
				add(people[p.person], {fi, p.face});
			}

			for (size_t k = 0; k < faces.size(); ++k)
			{
				if (placed[k] || faces[k].hash.is_empty()) continue;
				add(people.emplace_back(), {fi, k});
			}
		}

		// A face glimpsed once and unclear, where a frame read either side would have shown it again, is a passing
		// mistake of the detector's - a hand, a pattern - more often than a person.
		constexpr auto glimpse_s = 1.0;
		std::erase_if(people, [&](const person& p)
		{
			if (p.seen.size() > 1 || face_of(p.clearest).is_good()) return false;
			const auto fi = p.seen.front().frame;
			const auto near = [&](const size_t other)
			{
				return std::abs(frames[other].time - frames[fi].time) <= glimpse_s;
			};
			return (fi > 0 && near(fi - 1)) || (fi + 1 < frames.size() && near(fi + 1));
		});

		// The clearest people where a crowd holds more than a record keeps.
		std::ranges::stable_sort(people, [](const person& a, const person& b) { return a.clarity > b.clarity; });
		if (people.size() > static_cast<size_t>(t.max_faces)) people.resize(static_cast<size_t>(t.max_faces));
		// Then in the order they are first shown clearest: a frame's faces in the detector's stable order.
		std::ranges::stable_sort(people, [](const person& a, const person& b)
		{ return std::tie(a.clearest.frame, a.clearest.face) < std::tie(b.clearest.frame, b.clearest.face); });

		// A frame read is taken to show from halfway since the frame read before it to halfway to the one read after, and a
		// frame read without someone between two with them is a look away rather than a leave.
		const auto ends = length > 0.0 ? length : frames.back().time;
		const auto from = [&](const size_t fi)
		{
			return fi == 0 ? 0.0 : (frames[fi - 1].time + frames[fi].time) / 2.0;
		};
		const auto to = [&](const size_t fi)
		{
			return fi + 1 == frames.size() ? std::max(ends, frames[fi].time)
			                               : (frames[fi].time + frames[fi + 1].time) / 2.0;
		};

		for (const auto& p : people)
		{
			auto f = face_of(p.clearest);
			f.time_ms = to_ms(frames[p.clearest.frame].time);

			auto first = p.seen.front().frame;
			auto last = first;
			for (size_t i = 1; i <= p.seen.size(); ++i)
			{
				if (i < p.seen.size() && p.seen[i].frame <= last + 2)
				{
					last = p.seen[i].frame;
					continue;
				}

				f.seen.push_back({to_ms(from(first)), to_ms(to(last))});
				if (i < p.seen.size()) first = last = p.seen[i].frame;
			}

			result.faces.push_back(std::move(f));
		}

		return result;
	}
}
