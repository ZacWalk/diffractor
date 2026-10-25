// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: PNG files: decoded into a surface and encoded by libfiles, with their ICC profile, EXIF and
// XMP.

#include "pch.h"
#include "files.h"
#include "libfiles/encode.h"

ui::image_ptr save_png(const ui::const_surface_ptr& surface_in, const metadata_parts& metadata)
{
	if (!is_valid(surface_in)) return {};

	const auto dims = surface_in->dimensions();

	libfiles::image_view view;
	view.pixels = surface_in->pixels();
	view.stride = surface_in->stride();
	view.width = static_cast<uint32_t>(dims.cx);
	view.height = static_cast<uint32_t>(dims.cy);
	// Anything but RGB is written with its fourth byte as alpha.
	view.format = surface_in->format() == ui::texture_format::RGB
		              ? libfiles::pixel_format::bgrx8
		              : libfiles::pixel_format::bgra8;

	try
	{
		auto encoded = libfiles::encode_png(view, static_cast<uint16_t>(surface_in->orientation()),
		                                    embedded_metadata_of(metadata));

		// libpng's warnings go to the quiet debug log rather than the console and the test output.
		for (const auto& note : encoded.notes) df::log_once("libpng", note);

		return std::make_shared<ui::image>(df::blob(encoded.data.begin(), encoded.data.end()), dims,
		                                   ui::image_format::PNG, surface_in->orientation());
	}
	catch (const libfiles::encode_error& e)
	{
		df::log_once(__FUNCTION__, e.what());
		throw app_exception(e.what());
	}
}

namespace
{
	// A surface as the memory libfiles decodes a PNG into: every PNG as straight-alpha BGRA. Its size is
	// judged against the decode budget before any memory is taken, as the loader always judged it.
	class png_surface_sink final : public libfiles::image_sink
	{
	public:
		ui::surface_ptr surface;
		bool over_budget = false;

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			const sizei dimensions{static_cast<int>(frame.width), static_cast<int>(frame.height)};

			// The cspan decode path carries no budget gate of its own, and IHDR dimensions cost nothing to
			// declare: a small, highly compressible PNG can otherwise ask for gigabytes.
			if (reject_over_budget_source(nullptr, dimensions, "PNG"))
			{
				over_budget = true;
				return {};
			}

			surface = std::make_shared<ui::surface>();
			auto* const pixels = surface->alloc(dimensions, ui::texture_format::ARGB);
			if (!pixels) return {};
			return {pixels, surface->stride()};
		}
	};
}

ui::surface_ptr load_png(const df::cspan data)
{
	static constexpr uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

	if (data.size < 8 || !std::equal(std::begin(signature), std::end(signature), data.data))
	{
		throw app_exception("load_png invalid png header"s);
	}

	libfiles::reader r(std::span(data.data, data.size));
	png_surface_sink sink;
	const auto result = libfiles::decode(r, sink);

	// libpng's warnings - an iTXt with a damaged CRC, say - go to the quiet debug log rather than the
	// console, where they are still there for diagnosis.
	for (const auto& warning : result.warnings) df::log_once("libpng", warning);

	if (sink.over_budget) return {};

	if (!result.error.empty())
	{
		if (!result.truncated) df::log_once("load_png", result.error);
		throw app_exception(result.error);
	}

	// A surface that could not be allocated is returned empty, as it always was.
	if (sink.surface) sink.surface->orientation(static_cast<ui::orientation>(result.frame.orientation));
	return sink.surface;
}
