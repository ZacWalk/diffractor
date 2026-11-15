// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: HEIF/HEIC and AVIF decoding through libfiles, HEVC steered to FFmpeg's decoder, which is
// registered with libheif here: the primary image - shown with its profile's colours mapped to sRGB, or for
// an edit in its own colours, at sixteen bits, or as an HDR picture's signal - its gain map, and the thumbnail
// item that a scan wanting one cannot read without a decoder. And AVIF encoding, through libfiles' libheif and
// libaom, for a save, with a gain map where the photo has one: HEIC has no encoder Diffractor can ship.

#include "pch.h"
#include "files.h"
#include "libfiles/encode.h"

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
	core::texture_format texture_format_of(const libfiles::pixel_format format)
	{
		switch (format)
		{
		case libfiles::pixel_format::bgra8: return core::texture_format::ARGB;
		case libfiles::pixel_format::rgbx16: return core::texture_format::RGBX16;
		case libfiles::pixel_format::rgba16: return core::texture_format::RGBA16;
		default: return core::texture_format::RGB;
		}
	}

	// A surface as the memory libfiles decodes a HEIF into. The size the item declares is judged against
	// the decode budget before libheif decodes anything, at the bytes a pixel of the format takes; the surface
	// takes the size decoded, which a malformed file can make another.
	class heif_surface_sink final : public libfiles::image_sink
	{
		load_diagnostic* const _diagnostic;
		const std::string_view _format;

	public:
		core::surface_ptr surface;
		// Refused at sixteen bits a channel, where a byte a channel may yet fit.
		bool over_budget_wide = false;

		heif_surface_sink(load_diagnostic* const diagnostic, const std::string_view format) :
			_diagnostic(diagnostic), _format(format)
		{
		}

		bool accept(const libfiles::frame_info& declared) override
		{
			const sizei dimensions{static_cast<int>(declared.source_width), static_cast<int>(declared.source_height)};
			const auto format = texture_format_of(declared.format);

			if (core::is_wide(format) && files::exceeds_decode_budget(dimensions, core::bytes_per_pixel(format)))
			{
				over_budget_wide = true;
				return false;
			}

			return !reject_over_budget_source(_diagnostic, dimensions, _format);
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			auto s = std::make_shared<core::surface>();
			auto* const pixels = s->alloc(static_cast<int>(frame.width), static_cast<int>(frame.height),
			                              texture_format_of(frame.format));
			if (!pixels) return {};
			surface = std::move(s);
			return {pixels, surface->stride()};
		}
	};

	// A HEIF's primary image, its thumbnail item or its gain map, decoded by libfiles, HEVC named to FFmpeg's decoder.
	// An edit's decode takes sixteen bits a channel and an HDR picture's signal; map_to_srgb shows colours its profile
	// describes as sRGB's.
	libfiles::decode_result decode_with_libfiles(read_stream& s, libfiles::image_sink& sink, const bool thumbnail,
	                                             const bool edit = false, const bool map_to_srgb = false,
	                                             const bool gain_map = false)
	{
		read_stream_source source(s);
		libfiles::reader r(source);

		libfiles::decode_options options;
		if (const auto* const hevc = heif_hevc_decoder_id()) options.heif_hevc_decoder = hevc;
		options.heif_thumbnail = thumbnail;
		options.heif_gain_map = gain_map;
		options.allow_16bit = edit;
		options.keep_hdr = edit;
		options.map_to_srgb = map_to_srgb;
		return libfiles::decode(r, sink, options);
	}

	// alloc does not zero, so the row padding is cleared rather than uploaded uninitialised.
	void clear_row_padding(core::surface& s)
	{
		const auto row_bytes = static_cast<size_t>(s.dimensions().cx) * core::bytes_per_pixel(s.format());

		for (auto y = 0; y < s.dimensions().cy; ++y)
		{
			std::memset(s.pixels_line(y) + row_bytes, 0, s.stride() - row_bytes);
		}
	}

	// The primary image's first thumbnail item, decoded. It is a coded image of its own, so only a
	// decoder reads it, and decoding it is a full HEVC decode: a scan runs this only for a caller that
	// wants a thumbnail. It declares its own extent, so the primary's budget check says nothing about
	// it; refusing it costs the stand-in, not the scan. It is shown, so its colours are mapped to sRGB by the profile
	// the primary's are.
	void decode_heif_thumbnail(read_stream& s, file_scan_result& result)
	{
		heif_surface_sink sink(nullptr, "HEIF thumbnail");
		const auto decoded = decode_with_libfiles(s, sink, true, false, true);
		if (!decoded.success) return;

		clear_row_padding(*sink.surface);
		result.thumbnail_surface = std::move(sink.surface);
		// The thumbnail is a separate item with its own property list, so it is upright only if it
		// carries its own transform - the primary's does not reach it.
		result.thumbnail_orientation_applied = decoded.frame.transformed;
	}
}

file_scan_result scan_heif_libfiles(read_stream& s, const libfiles::scan_intent intent, const bool want_thumbnail,
                                    files* const decoder)
{
	still_scan_extras extras;
	extras.container_thumbnail = decode_heif_thumbnail;

	auto result = scan_still_libfiles(s, intent, want_thumbnail, decoder, extras);
	// The format the signature named, whatever the scan made of the rest.
	result.format = libfiles::image_format::heif;
	return result;
}

core::surface_ptr load_heif(read_stream& s, load_diagnostic* const diagnostic, picture_signal* const edit,
                            const bool map_to_srgb)
{
	heif_surface_sink sink(diagnostic, "HEIF");
	auto decoded = decode_with_libfiles(s, sink, false, edit != nullptr, map_to_srgb && !edit);

	if (edit && sink.over_budget_wide)
	{
		// Too large to edit at sixteen bits a channel or in HDR: at a byte a channel, an HDR picture shown as SDR,
		// as the display shows it.
		heif_surface_sink narrow(diagnostic, "HEIF");
		decoded = decode_with_libfiles(s, narrow, false);
		sink.surface = std::move(narrow.surface);
		edit->narrowed = true;
	}

	if (!decoded.success) return {};

	if (edit)
	{
		edit->tone_mapped = decoded.frame.tone_mapped;
		if (!decoded.frame.tone_mapped) edit->hdr = decoded.frame.hdr;
	}

	clear_row_padding(*sink.surface);
	// libheif has already turned the pixels for the item's own 'irot'/'imir', and libfiles reports the
	// EXIF orientation only where it has not, so the image is never turned twice. The scan path reports
	// top_left for these files, and the display agrees with it.
	sink.surface->orientation(static_cast<core::orientation>(decoded.frame.orientation));
	return sink.surface;
}

std::optional<gain_map_surface> load_heif_gain_map(read_stream& s, const bool probe)
{
	// The gain map judged against the decode budget as the photo is, and, for a probe, only found.
	struct gain_map_sink final : libfiles::image_sink
	{
		bool probe = false;
		bool found = false;
		core::surface_ptr surface;

		bool accept(const libfiles::frame_info& declared) override
		{
			found = true;
			return !probe && !reject_over_budget_source(nullptr,
			                                            {static_cast<int>(declared.source_width),
			                                             static_cast<int>(declared.source_height)},
			                                            "HEIF gain map");
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			auto map = std::make_shared<core::surface>();
			auto* const pixels = map->alloc(static_cast<int>(frame.width), static_cast<int>(frame.height),
			                                core::texture_format::RGB);
			if (!pixels) return {};
			surface = std::move(map);
			return {pixels, surface->stride()};
		}
	};

	gain_map_sink sink;
	sink.probe = probe;
	const auto decoded = decode_with_libfiles(s, sink, false, false, false, true);
	if (probe) return sink.found ? std::optional(gain_map_surface{}) : std::nullopt;
	if (!decoded.success || !decoded.frame.gain_map || !sink.surface) return {};

	clear_row_padding(*sink.surface);
	sink.surface->orientation(static_cast<core::orientation>(decoded.frame.orientation));
	return gain_map_surface{std::move(sink.surface), *decoded.frame.gain_map};
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

		libfiles::pixel_buffer allocate(const libfiles::frame_info&) override { return {}; }
	};

	choice sink;
	decode_with_libfiles(s, sink, false);
	// libfiles names only a registered decoder, and only FFmpeg's is named to it.
	return sink.decoder.empty() ? nullptr : heif_hevc_decoder_id();
}

// Packed pixels, a byte a channel or sixteen, as save_jxl takes them: a planar surface - a video frame - is converted
// to packed by its caller first. Sixteen bits are written as the bits the parameters ask for, an HDR picture's signal
// with its description, and a gain map beside the picture as ISO 21496-1 has one in a HEIF.
core::image_ptr save_avif(const core::const_surface_ptr& surface_in, const libfiles::metadata_blocks& metadata,
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

	libfiles::avif_options options;
	options.lossless = params.avif_lossless;
	options.quality = params.avif_quality;
	options.bit_depth = params.avif_bit_depth;

	if (params.gain_map && is_valid(params.gain_map->pixels) && core::is_packed(params.gain_map->pixels->format()))
	{
		const auto& map = *params.gain_map->pixels;
		options.gain_map = libfiles::gain_map_image{{map.pixels(), map.stride(), map.width(), map.height(),
		                                             libfiles::pixel_format::bgrx8},
		                                            params.gain_map->metadata};
	}

	libfiles::embedded_metadata embedded(metadata);
	embedded.hdr = params.hdr;

	auto encoded = libfiles::encode_avif(view, static_cast<uint16_t>(turn), embedded, options);
	for (const auto& note : encoded.notes) core::log(__FUNCTION__, note);
	if (encoded.data.empty()) return {};

	return std::make_shared<core::image>(core::blob(std::move(encoded.data)), dimensions, core::image_format::AVIF,
	                                     turn);
}
