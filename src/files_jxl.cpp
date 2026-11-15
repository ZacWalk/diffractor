// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: JPEG XL decoding and encoding through libfiles: decoding into a surface judged against the
// decode budget first - for an edit at sixteen bits, or as an HDR picture's signal - and encoding a surface
// with its metadata, and an HDR picture's description, for a save. libfiles scans these files too, including
// Brotli-compressed metadata boxes through its own decoder, and lists their structure for the verbose pane.

#include "pch.h"
#include "files.h"
#include "libfiles/encode.h"

file_scan_result scan_jxl_libfiles(read_stream& s, const libfiles::scan_intent intent, const bool want_thumbnail,
                                   files* const decoder)
{
	still_scan_extras extras;

	auto result = scan_still_libfiles(s, intent, want_thumbnail, decoder, extras);
	// The format the signature named, whatever the scan made of the rest.
	result.format = libfiles::image_format::jxl;
	return result;
}

namespace
{
	// A surface as the memory libfiles decodes a JPEG XL into, its size judged against the decode budget
	// first, at the bytes a pixel of the format takes, and the codestream's orientation carried on it.
	class jxl_surface_sink final : public libfiles::image_sink
	{
		load_diagnostic* const _diagnostic;

	public:
		core::surface_ptr surface;
		// Refused at sixteen bits a channel, where a byte a channel may yet fit.
		bool over_budget_wide = false;

		explicit jxl_surface_sink(load_diagnostic* const diagnostic) : _diagnostic(diagnostic) {}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			const sizei dimensions{static_cast<int>(frame.width), static_cast<int>(frame.height)};
			const auto format = frame.format == libfiles::pixel_format::rgba16   ? core::texture_format::RGBA16
			                    : frame.format == libfiles::pixel_format::rgbx16 ? core::texture_format::RGBX16
			                    : frame.format == libfiles::pixel_format::bgra8  ? core::texture_format::ARGB
			                                                                     : core::texture_format::RGB;

			if (core::is_wide(format) && files::exceeds_decode_budget(dimensions, core::bytes_per_pixel(format)))
			{
				over_budget_wide = true;
				return {};
			}

			if (reject_over_budget_source(_diagnostic, dimensions, "JXL")) return {};

			auto s = std::make_shared<core::surface>();
			auto* const pixels = s->alloc(dimensions, format, static_cast<core::orientation>(frame.orientation));
			if (!pixels) return {};
			surface = std::move(s);
			return {pixels, surface->stride()};
		}
	};

	libfiles::decode_result decode_jxl(const core::cspan data, jxl_surface_sink& sink, const bool edit)
	{
		libfiles::reader r{std::span<const uint8_t>(data.data, data.size)};
		libfiles::decode_options options;
		options.allow_16bit = edit;
		options.keep_hdr = edit;
		return libfiles::decode(r, sink, options);
	}
}

core::surface_ptr load_jxl(read_stream& s, load_diagnostic* const diagnostic, picture_signal* const edit)
{
	try
	{
		core::blob owned;
		const auto data = s.view_all(owned);

		jxl_surface_sink sink(diagnostic);
		auto decoded = decode_jxl(data, sink, edit != nullptr);

		if (edit && sink.over_budget_wide)
		{
			// Too large to edit at sixteen bits a channel or in HDR: at a byte a channel, an HDR picture shown as
			// SDR, as the display shows it.
			jxl_surface_sink narrow(diagnostic);
			decoded = decode_jxl(data, narrow, false);
			sink.surface = std::move(narrow.surface);
			edit->narrowed = true;
		}

		if (decoded.success)
		{
			if (edit)
			{
				edit->tone_mapped = decoded.frame.tone_mapped;
				if (!decoded.frame.tone_mapped) edit->hdr = decoded.frame.hdr;
			}

			return sink.surface;
		}
	}
	catch (std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
	}

	return {};
}

// Packed pixels, a byte a channel or sixteen: a planar surface - a video frame - is converted to packed by
// its caller first. The orientation, when given, is which way the pixels stand in place of the surface's own.
core::image_ptr save_jxl(const core::const_surface_ptr& surface_in, const libfiles::metadata_blocks& metadata,
                         const file_encode_params& params, const std::optional<core::orientation> orientation)
{
	if (!is_valid(surface_in)) return {};

	const auto dimensions = surface_in->dimensions();
	const auto turn = orientation.value_or(surface_in->orientation());

	libfiles::image_view view;
	view.pixels = surface_in->pixels();
	view.stride = surface_in->stride();
	view.width = static_cast<uint32_t>(dimensions.cx);
	view.height = static_cast<uint32_t>(dimensions.cy);

	switch (surface_in->format())
	{
	case core::texture_format::RGB: view.format = libfiles::pixel_format::bgrx8; break;
	case core::texture_format::ARGB: view.format = libfiles::pixel_format::bgra8; break;
	case core::texture_format::RGBX16: view.format = libfiles::pixel_format::rgbx16; break;
	case core::texture_format::RGBA16: view.format = libfiles::pixel_format::rgba16; break;
	default: return {};
	}

	libfiles::jxl_options options;
	options.lossless = params.jxl_lossless;
	options.quality = params.jxl_quality;

	libfiles::embedded_metadata embedded(metadata);
	embedded.hdr = params.hdr;

	auto encoded = libfiles::encode_jxl(view, static_cast<uint16_t>(turn), embedded, options);
	for (const auto& note : encoded.notes) core::log(__FUNCTION__, note);
	if (encoded.data.empty()) return {};

	return std::make_shared<core::image>(core::blob(std::move(encoded.data)), dimensions, core::image_format::JXL,
	                                     turn);
}
