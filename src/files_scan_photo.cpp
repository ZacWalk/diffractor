// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Still-image scanning. Every still format - JPEG, PNG, WebP, GIF, TIFF, PSD, BMP, HEIF and
// AVIF, and JPEG XL - is read by libfiles, whose result is mapped onto file_scan_result here; a format
// adds what libfiles leaves to its caller, decoding a HEIF thumbnail item or JPEG XL's Brotli. Embedded
// thumbnails are read and decoded only for a caller that wants one.

#include "pch.h"
#include "metadata_exif.h"
#include "files.h"
#include "libfiles/scan.h"

// Reached only for an embedded thumbnail whose format load_image_file cannot wrap, which needs a
// files instance to decode. Borrows the caller's rather than building a second libjpeg context.
static ui::surface_ptr decode_thumbnail(files* const decoder, const df::cspan data)
{
	if (decoder) return decoder->image_to_surface(data, {}, false, decode_intent::thumbnail);
	files ff;
	return ff.image_to_surface(data, {}, false, decode_intent::thumbnail);
}

namespace
{
	detected_format to_detected_format(const libfiles::image_format format)
	{
		switch (format)
		{
		case libfiles::image_format::jpeg: return detected_format::JPEG;
		case libfiles::image_format::png: return detected_format::PNG;
		case libfiles::image_format::webp: return detected_format::WEBP;
		case libfiles::image_format::gif: return detected_format::GIF;
		case libfiles::image_format::psd: return detected_format::PSD;
		case libfiles::image_format::bmp: return detected_format::BMP;
		case libfiles::image_format::tiff: return detected_format::TIFF;
		case libfiles::image_format::heif: return detected_format::HEIF;
		case libfiles::image_format::jxl: return detected_format::JXL;
		default: return detected_format::Unknown;
		}
	}

	df::blob to_blob(const std::vector<uint8_t>& bytes)
	{
		return {bytes.begin(), bytes.end()};
	}
}

metadata_kv_list to_metadata_kv(std::vector<libfiles::structure_row>& rows)
{
	metadata_kv_list result;
	result.reserve(rows.size());

	for (auto& row : rows)
	{
		auto& kv = result.emplace_back(std::move(row.key), std::move(row.value));
		kv.shape = std::move(row.shape);
		kv.id = std::move(row.id);
		kv.depth = row.depth;
		kv.container = row.container;
		kv.open_by_default = row.open_by_default;

		if (auto* const text = std::get_if<std::string>(&row.detail))
		{
			kv.detail = metadata_text_detail{std::move(*text)};
		}
		else if (auto* const bytes = std::get_if<std::vector<uint8_t>>(&row.detail))
		{
			kv.detail = metadata_binary_detail{std::move(*bytes)};
		}
		else if (auto* const table = std::get_if<libfiles::structure_table>(&row.detail))
		{
			kv.detail = metadata_numeric_detail{std::move(table->values), table->columns};
		}
	}

	return result;
}

namespace
{

	// An embedded thumbnail: JPEG, PNG and WebP stay encoded, anything else recognisable is decoded.
	void load_thumbnail(file_scan_result& result, const df::cspan data, files* const decoder)
	{
		const auto detected = files::detect_format(data);
		if (detected == detected_format::Unknown) return;

		if (is_image_format(detected)) result.thumbnail_image = load_image_file(data);
		else result.thumbnail_surface = decode_thumbnail(decoder, data);
	}

	// The Exif thumbnail, when wanted, and the orientation every thumbnail takes: an Exif thumbnail is
	// stored unrotated whatever the container did, and a decoded one needs it only where the decoder
	// did not already apply that item's own transform.
	void finish_with_exif(file_scan_result& result, const libfiles::byte_range thumbnail, const bool want_thumbnail,
	                      files* const decoder)
	{
		if (want_thumbnail && !thumbnail.empty())
		{
			load_thumbnail(result, {
				               result.metadata.exif.data() + thumbnail.offset, static_cast<size_t>(thumbnail.size)
			               }, decoder);
		}

		if (result.thumbnail_image) result.thumbnail_image->orientation(result.orientation);

		if (result.thumbnail_surface && !result.thumbnail_orientation_applied)
		{
			result.thumbnail_surface->orientation(result.orientation);
		}

		// The decoder already turned the pixels for a transform the container stored.
		if (result.orientation_applied)
		{
			result.orientation = ui::orientation::top_left;
		}
	}
}

file_scan_result scan_still_libfiles(read_stream& s, const scan_intent intent, const bool want_thumbnail,
                                     files* const decoder, const still_scan_extras& extras)
{
	file_scan_result result;

	if (s.size() < sizeof(pack128)) return result;

	try
	{
		read_stream_source source(s);
		const auto resident = s.view();
		std::optional<libfiles::reader> r;

		if (resident.empty()) r.emplace(source);
		else r.emplace(std::span(resident.data, resident.size));

		libfiles::scan_limits limits;
		limits.jpeg_photoshop_resources = files_test_hooks::jpeg_photoshop_resource_cap();
		limits.brotli = extras.brotli;

		auto scanned = libfiles::scan(*r, intent == scan_intent::inspect
			                                  ? libfiles::scan_intent::inspect
			                                  : libfiles::scan_intent::index, limits);

		if (!scanned.error.empty())
		{
			// A fault discards the whole scan, as a throw from a scanner always has.
			df::log(__FUNCTION__, scanned.error);
			return result;
		}

		result.format = to_detected_format(scanned.format);

		// BMP alone names its format only once its header has been read.
		if (scanned.format == libfiles::image_format::bmp && !scanned.success) result.format = detected_format::Unknown;

		result.width = scanned.width;
		result.height = scanned.height;
		if (!scanned.pixel_format.empty()) result.pixel_format = str::cache(scanned.pixel_format);
		if (scanned.orientation) result.orientation = static_cast<ui::orientation>(*scanned.orientation);

		result.metadata.xmp = to_blob(scanned.metadata.xmp);
		result.metadata.exif = to_blob(scanned.metadata.exif);
		result.metadata.iptc = to_blob(scanned.metadata.iptc);
		result.metadata.icc = to_blob(scanned.metadata.icc);
		result.metadata.photoshop_resources = to_blob(scanned.metadata.photoshop);
		result.tiff_metadata = std::move(scanned.tiff_metadata);
		result.structure_metadata = to_metadata_kv(scanned.structure);

		// A TIFF file's own GPS position, read as an EXIF block's is.
		if (scanned.gps)
		{
			const gps_coordinate coordinate(scanned.gps->latitude, scanned.gps->longitude);
			if (coordinate.is_valid()) result.gps = coordinate;
		}

		result.success = scanned.success;
		// A HEIF item's own rotation or mirror, which its decoder applies.
		result.orientation_applied = scanned.container_transform;

		// A thumbnail costs a read and a copy, so it is read only for a caller that wants one. The JFIF
		// extension thumbnail stands only where the Exif one does not.
		if (want_thumbnail)
		{
			if (extras.container_thumbnail) extras.container_thumbnail(s, result);

			if (!scanned.jfif_thumbnail.empty())
			{
				const auto bytes = r->read(scanned.jfif_thumbnail.offset, static_cast<size_t>(scanned.jfif_thumbnail.size));
				result.thumbnail_image = load_image_file({bytes.data(), bytes.size()});
			}

			if (!scanned.tiff_thumbnail.empty())
			{
				const auto bytes = r->read(scanned.tiff_thumbnail.offset, static_cast<size_t>(scanned.tiff_thumbnail.size));
				if (bytes.size() > 16) load_thumbnail(result, {bytes.data(), bytes.size()}, decoder);
			}
		}

		if (!result.metadata.exif.empty())
		{
			finish_with_exif(result, scanned.exif_thumbnail, want_thumbnail, decoder);
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}

	return result;
}

file_scan_result scan_photo_libfiles(read_stream& s, const scan_intent intent, const bool want_thumbnail,
                                     files* const decoder)
{
	return scan_still_libfiles(s, intent, want_thumbnail, decoder, {});
}

file_scan_result scan_photo(read_stream& s, const scan_intent intent, const bool want_thumbnail,
                            files* const decoder)
{
	file_scan_result result;

	// Empty or too small to contain a recognisable image signature.
	if (s.size() < sizeof(pack128)) return result;

	try
	{
		const auto* const still = find_still_format(files::detect_format(s.peek128(0)));

		if (still && still->scan)
		{
			result = still->scan(s, intent, want_thumbnail, decoder);
		}
	}
	catch (std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}

	return result;
}
