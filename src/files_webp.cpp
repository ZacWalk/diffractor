// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: WebP files: still images decoded into a surface and encoded by libfiles, with their ICC
// profile, EXIF and XMP.

#include "pch.h"
#include "files.h"
#include "libfiles/encode.h"

void files_test_hooks::fail_next_webp_chunk(const std::string_view fourcc)
{
	libfiles::test_hooks::fail_next_webp_chunk(fourcc);
}

namespace
{
	// A surface as the memory libfiles decodes a WebP into. Its size is judged against the decode budget
	// before any memory is taken: the source's, because that is what a hostile file declares, and
	// libwebp's 16383-pixel edge still permits a ~1 GB surface.
	class webp_surface_sink final : public libfiles::image_sink
	{
	public:
		ui::surface_ptr surface;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			const sizei source{static_cast<int>(frame.source_width), static_cast<int>(frame.source_height)};
			if (reject_over_budget_source(nullptr, source, "WEBP")) return {};

			const sizei dimensions{static_cast<int>(frame.width), static_cast<int>(frame.height)};
			auto s = std::make_shared<ui::surface>();
			uint8_t* pixels = nullptr;

			if (frame.format == libfiles::pixel_format::nv12)
			{
				// libfiles crops an odd edge, as decode_jpeg does, so an odd-sized image still reaches the
				// GPU as NV12: a 320x213 thumbnail would otherwise cost 4 bytes a pixel rather than 1.5.
				pixels = s->alloc(dimensions, ui::texture_format::NV12);
				s->color_space(frame.limited_range ? ui::color_space::rec601_limited : ui::color_space::rec601_full);
			}
			else
			{
				// Opaque images decode into the ignored X byte, so tag them RGB and let the renderer skip
				// blending.
				pixels = s->alloc(dimensions, frame.format == libfiles::pixel_format::bgra8
					                              ? ui::texture_format::ARGB
					                              : ui::texture_format::RGB);
			}

			if (!pixels) return {};
			surface = std::move(s);
			return {pixels, surface->stride()};
		}
	};
}

// libwebp rescales inside the decoder, so a reduced decode comes back at the size the caller asked for
// rather than at the file's size with a downscale still owed: the size ui::scale_dimensions gives, which
// files::fit_within uses, so it is a drop-in for decode-then-scale. NV12 is the caller's choice, and a
// reduced decode is packed regardless: NV12 exists to hand the renderer native planes.
ui::surface_ptr load_webp(const df::cspan data, const bool can_use_yuv, const sizei target_extent)
{
	libfiles::reader r(std::span(data.data, data.size));
	webp_surface_sink sink;

	libfiles::decode_options options;
	options.target_width = static_cast<uint32_t>(std::max(0, target_extent.cx));
	options.target_height = static_cast<uint32_t>(std::max(0, target_extent.cy));
	options.allow_nv12 = can_use_yuv;

	const auto result = libfiles::decode(r, sink, options);
	if (!result.success) return {};

	sink.surface->orientation(static_cast<ui::orientation>(result.frame.orientation));
	return sink.surface;
}

ui::image_ptr save_webp(const ui::const_surface_ptr& surface_in, const metadata_parts& metadata,
                        const file_encode_params& params)
{
	if (!is_valid(surface_in)) return {};

	const auto dimensions = surface_in->dimensions();
	const auto cs = surface_in->color_space();

	libfiles::image_view view;
	view.pixels = surface_in->pixels();
	view.stride = surface_in->stride();
	view.width = static_cast<uint32_t>(dimensions.cx);
	view.height = static_cast<uint32_t>(dimensions.cy);

	switch (surface_in->format())
	{
	case ui::texture_format::RGB:
		view.format = libfiles::pixel_format::bgrx8;
		break;
	case ui::texture_format::ARGB:
		view.format = libfiles::pixel_format::bgra8;
		break;
	case ui::texture_format::NV12:
		// Only the matrix VP8 assumes: a rec709 or rec2020 frame would be encoded as if it were 601.
		if (cs != ui::color_space::rec601_full && cs != ui::color_space::rec601_limited) return {};
		view.format = libfiles::pixel_format::nv12;
		view.limited_range = cs == ui::color_space::rec601_limited;
		break;
	default:
		return {};
	}

	libfiles::webp_options options;
	options.lossless = params.webp_lossless;
	options.quality = params.webp_quality;
	options.lossy_alpha = params.webp_lossy_alpha;
	options.fast = params.webp_fast;

	const auto encoded = libfiles::encode_webp(view, static_cast<uint16_t>(surface_in->orientation()),
	                                           embedded_metadata_of(metadata), options);
	for (const auto& note : encoded.notes) df::log(__FUNCTION__, note);
	if (encoded.data.empty()) return {};

	return std::make_shared<ui::image>(df::cspan(encoded.data.data(), encoded.data.size()), dimensions,
	                                   ui::image_format::WEBP, surface_in->orientation());
}
