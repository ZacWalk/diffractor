// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The still image formats libfiles names from their signatures, one entry each: the file
// type it registers, how its header is scanned without decoding, how a file of it loads, and how
// in-memory bytes of it decode. Scanning, loading and decoding all dispatch through this table.

#include "pch.h"
#include "files.h"

namespace
{
	ui::surface_ptr fitted(files& ff, ui::surface_ptr loaded, const still_decode_request& request)
	{
		return is_valid(loaded) ? ff.fit_within(std::move(loaded), request.target_extent) : ui::surface_ptr{};
	}

	// These decoders report a malformed file by throwing. The failure is logged and answered with
	// nothing, so the caller still offers the bytes to ffmpeg.
	template <typename Decode>
	ui::surface_ptr decode_or_log(Decode&& decode)
	{
		try
		{
			return decode();
		}
		catch (const std::exception& e)
		{
			df::log("files::image_to_surface", e.what());
			return {};
		}
	}

	constexpr auto xmp_edit = file_traits::embedded_xmp | file_traits::edit | file_traits::in_place_metadata;

	constexpr still_format s_still_formats[] = {
		{
			.format = detected_format::PSD,
			.extensions = "psd",
			.description = "Adobe Photoshop Drawing",
			.traits = xmp_edit,
			.scan = scan_photo_libfiles,
			.load = [](read_stream& s, load_diagnostic* const d) { return load_psd(s, d); },
			.decode = [](files& ff, const df::cspan data, const still_decode_request& r)
			{
				mem_read_stream stream(data);
				return fitted(ff, load_psd(stream), r);
			},
		},
		{
			.format = detected_format::GIF,
			.extensions = "gif,giff",
			.description = "CompuServe's Graphics Interchange Format",
			.traits = xmp_edit,
			.scan = scan_photo_libfiles,
		},
		{
			.format = detected_format::JXL,
			.extensions = "jxl",
			.description = "JPEG XL",
			.traits = file_traits::embedded_xmp,
			.scan = scan_jxl_libfiles,
			.load = [](read_stream& s, load_diagnostic* const d) { return load_jxl(s, d); },
			.decode = [](files& ff, const df::cspan data, const still_decode_request& r)
			{
				return decode_or_log([&ff, data, &r]
				{
					mem_read_stream stream(data);
					return fitted(ff, load_jxl(stream), r);
				});
			},
		},
		{
			.format = detected_format::WEBP,
			.extensions = "webp",
			.traits = xmp_edit,
			.encoded = ui::image_format::WEBP,
			.scan = scan_photo_libfiles,
			.decode = [](files& ff, const df::cspan data, const still_decode_request& r)
			{
				return decode_or_log([&ff, data, &r]
				{
					return fitted(ff, load_webp(data, r.can_use_yuv, r.target_extent), r);
				});
			},
		},
		{
			.format = detected_format::JPEG,
			.extensions = "jpeg,jpg,jpe,jfif",
			.description = "Joint Photographic Experts Group",
			.traits = xmp_edit,
			.encoded = ui::image_format::JPEG,
			.scan = scan_photo_libfiles,
			.decode = decode_still_jpeg,
		},
		{
			.format = detected_format::BMP,
			.extensions = "bmp",
			.description = "Microsoft Windows Bitmap",
			.scan = scan_photo_libfiles,
		},
		{
			.format = detected_format::PNG,
			.extensions = "png",
			.description = "Portable Network Graphic",
			.traits = xmp_edit,
			.encoded = ui::image_format::PNG,
			.scan = scan_photo_libfiles,
			.decode = [](files& ff, const df::cspan data, const still_decode_request& r)
			{
				return decode_or_log([&ff, data, &r] { return fitted(ff, load_png(data), r); });
			},
		},
		{
			.format = detected_format::TIFF,
			.extensions = "tiff, tif",
			.description = "Tagged Image File Format",
			.traits = xmp_edit,
			.scan = scan_photo_libfiles,
		},
		{
			.format = detected_format::HEIF,
			.extensions = "heif, heifs, heic, heics, avci, avcs, avif, avifs",
			.description = "High Efficiency Image File Format",
			.traits = file_traits::embedded_xmp,
			.scan = scan_heif_libfiles,
			.load = [](read_stream& s, load_diagnostic* const d) { return load_heif(s, d); },
			.decode = [](files& ff, const df::cspan data, const still_decode_request& r)
			{
				return decode_or_log([&ff, data, &r]
				{
					mem_read_stream stream(data);
					return fitted(ff, load_heif(stream), r);
				});
			},
		},
	};
}

// A JPEG decodes at a reduced scale already. Planar YUV for display goes to the sampler as it is,
// which resizes at draw time; a thumbnail is about to be encoded, so it is reduced here - and stays
// planar, because that is the form VP8 wants.
ui::surface_ptr decode_still_jpeg(files& ff, const df::cspan data, const still_decode_request& request)
{
	bool is_yuv = false;
	auto decoded = ff.decode_jpeg(data, request.target_extent, request.can_use_yuv, {}, is_yuv, {}, request.intent);

	if (!is_valid(decoded)) return {};

	return is_yuv && request.intent != decode_intent::thumbnail
		       ? std::move(decoded)
		       : ff.fit_within(std::move(decoded), request.target_extent);
}

std::span<const still_format> still_formats()
{
	return s_still_formats;
}

const still_format* find_still_format(const detected_format format)
{
	for (const auto& f : s_still_formats)
	{
		if (f.format == format) return &f;
	}

	return nullptr;
}

// Every still format is a photo.
file_type still_file_type(const detected_format format)
{
	const auto* const f = find_still_format(format);
	df::assert_true(f != nullptr);
	return f ? file_type(file_group::photo, f->extensions, f->description, f->traits) : file_type::other;
}
