// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: JPEG XL decoding through libfiles, into a surface judged against the decode budget first.
// libfiles scans these files too; this supplies the Brotli decoder its scan asks for when a container
// keeps its metadata boxes compressed.

#include "pch.h"
#include "files.h"
#include "files_jpeg.h"

#include <brotli/decode.h>

// The first buffer a compressed metadata box is decompressed into, grown as far as libfiles allows.
constexpr size_t box_chunk = 64u * 1024u;

// What libfiles asks for when a JPEG XL keeps a metadata box compressed ('brob'): the box's one whole
// Brotli stream, decompressed into at most max_size bytes. A stream that would need more is abandoned
// there rather than decompressed to the end.
static std::optional<std::vector<uint8_t>> decompress_brotli(const std::span<const uint8_t> compressed,
                                                             std::string_view, const size_t max_size)
{
	const df::releaser<BrotliDecoderState> dec(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr),
	                                           [](auto* d) { BrotliDecoderDestroyInstance(d); });
	if (!dec.get()) return {};

	std::vector<uint8_t> out(std::min(max_size, box_chunk));
	auto available_in = compressed.size();
	const auto* next_in = compressed.data();
	size_t produced = 0;

	for (;;)
	{
		auto available_out = out.size() - produced;
		auto* next_out = out.data() + produced;
		const auto status = BrotliDecoderDecompressStream(dec.get(), &available_in, &next_in, &available_out,
		                                                  &next_out, nullptr);
		produced = out.size() - available_out;

		if (status == BROTLI_DECODER_RESULT_SUCCESS)
		{
			// One whole stream and nothing after it, as libjxl requires of a 'brob' box.
			if (available_in != 0) return {};
			out.resize(produced);
			return out;
		}

		if (status != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT || out.size() >= max_size) return {};
		out.resize(std::min(max_size, out.size() * 2u));
	}
}

file_scan_result scan_jxl_libfiles(read_stream& s, scan_intent, const bool want_thumbnail, files* const decoder)
{
	still_scan_extras extras;
	extras.brotli = decompress_brotli;

	// Scanned for indexing whatever the intent: libjxl's scan gave JPEG XL no structure report.
	auto result = scan_still_libfiles(s, scan_intent::index, want_thumbnail, decoder, extras);
	// The format the signature named, whatever the scan made of the rest.
	result.format = detected_format::JXL;
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
		ui::surface_ptr surface;

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

			auto s = std::make_shared<ui::surface>();
			auto* const pixels = s->alloc(static_cast<int>(frame.width), static_cast<int>(frame.height),
			                              frame.format == libfiles::pixel_format::bgra8
				                              ? ui::texture_format::ARGB
				                              : ui::texture_format::RGB,
			                              static_cast<ui::orientation>(frame.orientation));
			if (!pixels) return {};
			surface = std::move(s);
			return {pixels, surface->stride()};
		}
	};
}

ui::surface_ptr load_jxl(read_stream& s, load_diagnostic* const diagnostic)
{
	try
	{
		df::blob owned;
		const auto data = s.view_all(owned);
		libfiles::reader r{std::span<const uint8_t>(data.data, data.size)};

		jxl_surface_sink sink(diagnostic);
		const auto decoded = libfiles::decode(r, sink);
		if (decoded.success) return sink.surface;
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}

	return {};
}
