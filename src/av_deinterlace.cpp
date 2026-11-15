// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Interlaced video made progressive on the CPU, a field at a time. A line between a field's own is
// predicted from the other field's lines in the frames either side of the instant the field was taken, and
// held to that prediction as closely as the motion around it allows: where nothing moved it is the other
// field's line, at full vertical detail, and where something did it is made from the field's own lines. The
// motion test is yadif's and the filters are the Weston three-field deinterlacer's (BBC R&D White Paper
// WHP 102), as FFmpeg's bwdif combines them. Only the frame before is read, so no picture waits for the
// frame after it.

#include "pch.h"
#include "av_deinterlace.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244) // FFmpeg's C clipping helpers in libavutil/common.h narrow an int implicitly
#endif
extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace
{
	// Out of 8192. Across a field's own lines, the rows either side and the rows beyond them - near and far -
	// as a low-pass beside the other field's detail, or alone where the other field cannot be used. Across
	// the other field's lines, a high-pass from the row itself and the rows two and four away, which brings
	// back the vertical detail a field alone cannot hold. Over a flat picture each low-pass sums to 8192 and
	// the high-pass to nothing.
	constexpr int low_near = 4309;
	constexpr int low_far = 213;
	constexpr int high_0 = 5570;
	constexpr int high_2 = 3801;
	constexpr int high_4 = 1016;
	constexpr int alone_near = 5077;
	constexpr int alone_far = 981;

	// The row a field holds nearest y: y itself inside the plane, and past its top or bottom that field's
	// nearest row, so a filter reaching over an edge reads the field it meant to.
	int field_row(const int y, const int height)
	{
		if (y < 0) return y & 1;
		if (y >= height) return height - 1 - (((height - 1) ^ y) & 1);
		return y;
	}

	struct plane_view
	{
		const uint8_t* data = nullptr;
		ptrdiff_t stride = 0;

		template <typename T> const T* row(const int y) const { return std::bit_cast<const T*>(data + stride * y); }
	};

	struct plane_job
	{
		plane_view current;
		// No rows where there is no frame before.
		plane_view previous;
		uint8_t* out = nullptr;
		ptrdiff_t out_stride = 0;
		int samples = 0;
		int height = 0;
		// The largest value a sample holds: 255, or 1023 shifted up into P010's top ten bits.
		int top = 0;
	};

	// One plane of a field made whole: the field's own rows - the even ones where kept is 0, the odd where it is
	// 1 - copied, and the rows between made. Those are predicted from the other field: for the first field, its
	// rows in the frame before and in this one, a field period either side of the instant shown; for the second,
	// its rows in this frame alone, a field period before.
	template <typename T> void fill_plane(const plane_job& job, const int kept, const bool second_field)
	{
		const auto height = job.height;
		const auto has_previous = job.previous.data != nullptr;
		const auto& current = job.current;
		const auto& earlier = second_field ? current : job.previous;

		for (auto y = 0; y < height; ++y)
		{
			auto* const dst = std::bit_cast<T*>(job.out + job.out_stride * y);
			const auto* const own = current.row<T>(y);

			if ((y & 1) == kept || height < 2)
			{
				memcpy(dst, own, sizeof(T) * job.samples);
				continue;
			}

			const auto at = [y, height](const plane_view& plane, const int offset)
			{
				return plane.row<T>(field_row(y + offset, height));
			};

			const T* const up = at(current, -1);
			const T* const down = at(current, 1);
			const T* const up3 = at(current, -3);
			const T* const down3 = at(current, 3);

			if (!has_previous)
			{
				for (auto x = 0; x < job.samples; ++x)
				{
					const auto value = (alone_near * (up[x] + down[x]) - alone_far * (up3[x] + down3[x])) >> 13;
					dst[x] = static_cast<T>(std::clamp(value, 0, job.top));
				}

				continue;
			}

			const T* const a0 = at(earlier, 0);
			const T* const a2 = at(earlier, -2);
			const T* const a2_below = at(earlier, 2);
			const T* const a4 = at(earlier, -4);
			const T* const a4_below = at(earlier, 4);
			const T* const b2 = at(current, -2);
			const T* const b2_below = at(current, 2);
			const T* const b4 = at(current, -4);
			const T* const b4_below = at(current, 4);
			const T* const before = at(job.previous, 0);
			const T* const before_up = at(job.previous, -1);
			const T* const before_down = at(job.previous, 1);

			for (auto x = 0; x < job.samples; ++x)
			{
				const int c = up[x];
				const int e = down[x];
				const int across = a0[x] + own[x];
				const int predicted = across >> 1;
				// What moved from the frame before to this one: the other field's line here, and the field's own
				// lines either side of it.
				const int moved_here = std::abs(before[x] - own[x]);
				const int moved_beside = (std::abs(before_up[x] - c) + std::abs(before_down[x] - e)) >> 1;
				int allowed = std::max(moved_here >> 1, moved_beside);

				if (allowed == 0)
				{
					dst[x] = static_cast<T>(predicted);
					continue;
				}

				// Where the prediction does not lie between the rows around it as the other field's rows two away
				// do, the line is let further from it.
				const int above = ((a2[x] + b2[x]) >> 1) - c;
				const int below = ((a2_below[x] + b2_below[x]) >> 1) - e;
				const int from_up = predicted - c;
				const int from_down = predicted - e;
				const int most = std::max(std::max(from_down, from_up), std::min(above, below));
				const int least = std::min(std::min(from_down, from_up), std::max(above, below));
				allowed = std::max(std::max(allowed, least), -most);

				int value;

				if (std::abs(c - e) > moved_here)
				{
					const int near_other = a2[x] + b2[x] + a2_below[x] + b2_below[x];
					const int far_other = a4[x] + b4[x] + a4_below[x] + b4_below[x];
					value = (((high_0 * across - high_2 * near_other + high_4 * far_other) >> 2) + low_near * (c + e) -
					         low_far * (up3[x] + down3[x])) >>
					        13;
				}
				else
				{
					value = (alone_near * (c + e) - alone_far * (up3[x] + down3[x])) >> 13;
				}

				value = std::clamp(value, predicted - allowed, predicted + allowed);
				dst[x] = static_cast<T>(std::clamp(value, 0, job.top));
			}
		}
	}
}

bool av_can_deinterlace(const int pixel_format)
{
	const auto* const desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(pixel_format));
	if (!desc || desc->nb_components < 1) return false;

	constexpr uint64_t refused = AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM |
	                             AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BAYER | AV_PIX_FMT_FLAG_FLOAT;
	if ((desc->flags & refused) != 0) return false;

	// A packed layout shares a plane between luma and chroma - YUYV - or a word between samples.
	if (desc->nb_components > 1 && (desc->flags & AV_PIX_FMT_FLAG_PLANAR) == 0) return false;

	for (auto c = 0; c < desc->nb_components; ++c)
	{
		if (desc->comp[c].depth + desc->comp[c].shift > 16) return false;
	}

	return true;
}

bool av_is_interlaced(const AVFrame& frame)
{
	return (frame.flags & AV_FRAME_FLAG_INTERLACED) != 0;
}

bool av_deinterlace_field(const AVFrame& current, const AVFrame* previous, const bool second_field, AVFrame& out)
{
	const auto format = static_cast<AVPixelFormat>(current.format);
	if (!av_can_deinterlace(format) || current.width < 1 || current.height < 1 || !current.data[0]) return false;

	// A frame of another shape - where the stream changed size - says nothing of what moved in this one.
	if (previous && (previous->format != current.format || previous->width != current.width ||
	                 previous->height != current.height || !previous->data[0]))
	{
		previous = nullptr;
	}

	av_frame_unref(&out);
	out.format = current.format;
	out.width = current.width;
	out.height = current.height;

	if (av_frame_get_buffer(&out, 0) < 0 || av_frame_copy_props(&out, &current) < 0)
	{
		av_frame_unref(&out);
		return false;
	}

	out.flags &= ~(AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST);

	const auto* const desc = av_pix_fmt_desc_get(format);
	// The field shown first holds the even rows where the top field leads, and the odd where the bottom does.
	const auto first_rows = (current.flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0 ? 0 : 1;
	const auto kept = second_field ? 1 - first_rows : first_rows;

	for (auto plane = 0; plane < av_pix_fmt_count_planes(format); ++plane)
	{
		// The first component a plane holds says how deep its samples are, and whether it is chroma, which a
		// layout may hold at fewer rows.
		auto component = 0;
		while (component < desc->nb_components && desc->comp[component].plane != plane) ++component;
		if (component == desc->nb_components) continue;

		const auto& comp = desc->comp[component];
		const auto bytes = comp.depth + comp.shift > 8 ? 2 : 1;
		const auto row_bytes = av_image_get_linesize(format, current.width, plane);
		if (row_bytes <= 0) continue;

		plane_job job;
		job.current = {current.data[plane], current.linesize[plane]};
		if (previous) job.previous = {previous->data[plane], previous->linesize[plane]};
		job.out = out.data[plane];
		job.out_stride = out.linesize[plane];
		job.samples = row_bytes / bytes;
		job.height = component == 1 || component == 2 ? AV_CEIL_RSHIFT(current.height, desc->log2_chroma_h)
		                                              : current.height;
		job.top = ((1 << comp.depth) - 1) << comp.shift;

		if (bytes == 1) fill_plane<uint8_t>(job, kept, second_field);
		else fill_plane<uint16_t>(job, kept, second_field);
	}

	return true;
}
