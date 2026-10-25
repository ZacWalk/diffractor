// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: HEIF/HEIC and AVIF decoding through libfiles, HEVC steered to FFmpeg's decoder, which is
// registered with libheif here: the primary image, and the thumbnail item that a scan wanting one cannot
// read without a decoder.

#include "pch.h"
#include "files.h"

#define LIBHEIF_STATIC_BUILD 1

#include <libheif/heif.h>
#include <libheif/heif_plugin.h>

// libheif's FFmpeg decoder plugin, which cmake/DiffractorHeif.cmake builds from libfiles' libheif: its
// header, plugins/decoder_ffmpeg.h, brings libheif's internal headers with it.
const heif_decoder_plugin* get_decoder_plugin_ffmpeg();

// libheif's plugin priority picks libde265 for HEVC. FFmpeg's HEVC decoder, linked for video anyway,
// decodes the same pixels faster: a 4000x2252 photograph in 40 tiles took 105 ms against 151 ms, and
// 264 ms against 463 ms on one thread. It is also correct where libde265 is not: libde265's 32-bit
// build is not bit-exact, and corrupts blocks of a photograph that both 64-bit decoders agree on.
// libfiles' libheif has no FFmpeg of its own, so the plugin is registered here, once, before libheif
// decodes anything through it - every decode here asks this first - and libheif keeps it until
// heif_deinit, which nothing calls. Null if libheif refused it, which leaves libheif's own choice.
const char* heif_hevc_decoder_id()
{
	static const char* const id = []() -> const char*
	{
		heif_register_decoder_plugin(get_decoder_plugin_ffmpeg());

		std::array<const heif_decoder_descriptor*, 16> descriptors = {};
		const auto count = heif_get_decoder_descriptors(heif_compression_HEVC, descriptors.data(),
		                                                static_cast<int>(descriptors.size()));

		for (auto i = 0; i < count; ++i)
		{
			const auto* const name = heif_decoder_descriptor_get_id_name(descriptors[i]);
			if (name && std::string_view(name) == "ffmpeg") return "ffmpeg";
		}

		return nullptr;
	}();

	return id;
}

namespace
{
	// A surface as the memory libfiles decodes a HEIF into. The size the item declares is judged against
	// the decode budget before libheif decodes anything; the surface takes the size decoded, which a
	// malformed file can make another.
	class heif_surface_sink final : public libfiles::image_sink
	{
		load_diagnostic* const _diagnostic;
		const std::string_view _format;

	public:
		ui::surface_ptr surface;

		heif_surface_sink(load_diagnostic* const diagnostic, const std::string_view format) :
			_diagnostic(diagnostic), _format(format)
		{
		}

		bool accept(const libfiles::frame_info& declared) override
		{
			return !reject_over_budget_source(_diagnostic, {
				                                  static_cast<int>(declared.source_width),
				                                  static_cast<int>(declared.source_height)
			                                  }, _format);
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			auto s = std::make_shared<ui::surface>();
			auto* const pixels = s->alloc(static_cast<int>(frame.width), static_cast<int>(frame.height),
			                              frame.format == libfiles::pixel_format::bgra8
				                              ? ui::texture_format::ARGB
				                              : ui::texture_format::RGB);
			if (!pixels) return {};
			surface = std::move(s);
			return {pixels, surface->stride()};
		}
	};

	// A HEIF's primary image or its thumbnail item, decoded by libfiles, HEVC named to FFmpeg's decoder.
	libfiles::decode_result decode_with_libfiles(read_stream& s, libfiles::image_sink& sink, const bool thumbnail)
	{
		read_stream_source source(s);
		libfiles::reader r(source);

		libfiles::decode_options options;
		if (const auto* const hevc = heif_hevc_decoder_id()) options.heif_hevc_decoder = hevc;
		options.heif_thumbnail = thumbnail;
		return libfiles::decode(r, sink, options);
	}

	// alloc does not zero, so the row padding is cleared rather than uploaded uninitialised.
	void clear_row_padding(ui::surface& s)
	{
		const auto row_bytes = static_cast<size_t>(s.dimensions().cx) * 4u;

		for (auto y = 0; y < s.dimensions().cy; ++y)
		{
			std::memset(s.pixels_line(y) + row_bytes, 0, s.stride() - row_bytes);
		}
	}

	// The primary image's first thumbnail item, decoded. It is a coded image of its own, so only a
	// decoder reads it, and decoding it is a full HEVC decode: a scan runs this only for a caller that
	// wants a thumbnail. It declares its own extent, so the primary's budget check says nothing about
	// it; refusing it costs the stand-in, not the scan.
	void decode_heif_thumbnail(read_stream& s, file_scan_result& result)
	{
		heif_surface_sink sink(nullptr, "HEIF thumbnail");
		const auto decoded = decode_with_libfiles(s, sink, true);
		if (!decoded.success) return;

		clear_row_padding(*sink.surface);
		result.thumbnail_surface = std::move(sink.surface);
		// The thumbnail is a separate item with its own property list, so it is upright only if it
		// carries its own transform - the primary's does not reach it.
		result.thumbnail_orientation_applied = decoded.frame.transformed;
	}
}

file_scan_result scan_heif_libfiles(read_stream& s, const scan_intent intent, const bool want_thumbnail,
                                    files* const decoder)
{
	still_scan_extras extras;
	extras.container_thumbnail = decode_heif_thumbnail;

	auto result = scan_still_libfiles(s, intent, want_thumbnail, decoder, extras);
	// The format the signature named, whatever the scan made of the rest.
	result.format = detected_format::HEIF;
	return result;
}

ui::surface_ptr load_heif(read_stream& s, load_diagnostic* const diagnostic)
{
	heif_surface_sink sink(diagnostic, "HEIF");
	const auto decoded = decode_with_libfiles(s, sink, false);
	if (!decoded.success) return {};

	clear_row_padding(*sink.surface);
	// libheif has already turned the pixels for the item's own 'irot'/'imir', and libfiles reports the
	// EXIF orientation only where it has not, so the image is never turned twice. The scan path reports
	// top_left for these files, and the display agrees with it.
	sink.surface->orientation(static_cast<ui::orientation>(decoded.frame.orientation));
	return sink.surface;
}

const char* heif_primary_decoder_id(read_stream& s)
{
	// A sink that takes the decoder libfiles chose and refuses the image before it is decoded.
	struct choice final : libfiles::image_sink
	{
		std::string decoder;

		bool accept(const libfiles::frame_info& declared) override
		{
			decoder = declared.heif_decoder;
			return false;
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info&) override
		{
			return {};
		}
	};

	choice sink;
	decode_with_libfiles(s, sink, false);
	// libfiles names only a registered decoder, and only FFmpeg's is named to it.
	return sink.decoder.empty() ? nullptr : heif_hevc_decoder_id();
}
