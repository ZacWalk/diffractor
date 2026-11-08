// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: JPEG XL decoding and encoding through libfiles: decoding into a surface judged against the
// decode budget first, and encoding a surface with its metadata for a save. libfiles scans these files
// too, including Brotli-compressed metadata boxes through its own decoder.

#include "pch.h"
#include "files.h"
#include "libfiles/encode.h"

file_scan_result scan_jxl_libfiles(read_stream& s, libfiles::scan_intent, const bool want_thumbnail, files* const decoder)
{
	still_scan_extras extras;

	// Scanned for indexing whatever the intent: libjxl's scan gave JPEG XL no structure report.
	auto result = scan_still_libfiles(s, libfiles::scan_intent::index, want_thumbnail, decoder, extras);
	// The format the signature named, whatever the scan made of the rest.
	result.format = libfiles::image_format::jxl;
	return result;
}

namespace
{
	// A surface as the memory libfiles decodes a JPEG XL into, its size judged against the decode budget
	// first, and the codestream's orientation carried on it.
	class jxl_surface_sink final : public libfiles::image_sink
	{
		load_diagnostic* const _diagnostic;

	public:
		core::surface_ptr surface;

		explicit jxl_surface_sink(load_diagnostic* const diagnostic) : _diagnostic(diagnostic)
		{
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			if (reject_over_budget_source(_diagnostic, {static_cast<int>(frame.width), static_cast<int>(frame.height)},
			                              "JXL"))
			{
				return {};
			}

			auto s = std::make_shared<core::surface>();
			auto* const pixels = s->alloc(static_cast<int>(frame.width), static_cast<int>(frame.height),
			                              frame.format == libfiles::pixel_format::bgra8
				                              ? core::texture_format::ARGB
				                              : core::texture_format::RGB,
			                              static_cast<core::orientation>(frame.orientation));
			if (!pixels) return {};
			surface = std::move(s);
			return {pixels, surface->stride()};
		}
	};
}

core::surface_ptr load_jxl(read_stream& s, load_diagnostic* const diagnostic)
{
	try
	{
		core::blob owned;
		const auto data = s.view_all(owned);
		libfiles::reader r{std::span<const uint8_t>(data.data, data.size)};

		jxl_surface_sink sink(diagnostic);
		const auto decoded = libfiles::decode(r, sink);
		if (decoded.success) return sink.surface;
	}
	catch (std::exception& e)
	{
		core::log(__FUNCTION__, e.what());
	}

	return {};
}

// Packed pixels only: a planar surface - a video frame - is converted to packed by its caller first.
// The orientation, when given, is which way the pixels stand in place of the surface's own.
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
	case core::texture_format::RGB:
		view.format = libfiles::pixel_format::bgrx8;
		break;
	case core::texture_format::ARGB:
		view.format = libfiles::pixel_format::bgra8;
		break;
	default:
		return {};
	}

	libfiles::jxl_options options;
	options.lossless = params.jxl_lossless;
	options.quality = params.jxl_quality;

	auto encoded = libfiles::encode_jxl(view, static_cast<uint16_t>(turn), metadata,
	                                          options);
	for (const auto& note : encoded.notes) core::log(__FUNCTION__, note);
	if (encoded.data.empty()) return {};

	return std::make_shared<core::image>(core::blob(std::move(encoded.data)), dimensions,
	                                   core::image_format::JXL, turn);
}
