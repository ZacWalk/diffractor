// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Adobe Photoshop (PSD) files: the composite image, decoded by libfiles into a surface.

#include "pch.h"
#include "files.h"

#include "libfiles/decode.h"

namespace
{
	// A surface as the memory libfiles decodes into, so the image lands in it with nothing copied. Its
	// size is judged against the decode budget before any memory is taken, as the loaders judge it.
	class surface_sink final : public libfiles::image_sink
	{
		load_diagnostic* const _diagnostic;
		const std::string_view _format;

	public:
		ui::surface_ptr surface;

		surface_sink(load_diagnostic* const diagnostic, const std::string_view format) : _diagnostic(diagnostic),
			_format(format)
		{
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			if (frame.format != libfiles::pixel_format::bgrx8) return {};

			const sizei dimensions{static_cast<int>(frame.width), static_cast<int>(frame.height)};
			if (reject_over_budget_source(_diagnostic, dimensions, _format)) return {};

			auto s = std::make_shared<ui::surface>();
			if (!s->alloc(dimensions, ui::texture_format::RGB)) return {};
			surface = std::move(s);
			return {surface->pixels(), surface->stride()};
		}
	};
}

ui::surface_ptr load_psd(read_stream& s, load_diagnostic* const diagnostic)
{
	read_stream_source source(s);
	libfiles::reader r(source);
	surface_sink sink(diagnostic, "PSD");
	const auto result = libfiles::decode(r, sink);

	// A file cut short throws, as this loader always has, so a damaged PSD is not offered to ffmpeg.
	if (result.truncated) throw app_exception(result.error);
	return result.success ? std::move(sink.surface) : ui::surface_ptr{};
}
