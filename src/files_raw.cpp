// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Camera raw files, through libfiles, which opens them with LibRaw: a load - the largest preview,
// or the raw data developed - into a surface judged against the decode budget first, and a scan - the
// metadata LibRaw parses, the listing the info pane shows, and a thumbnail - mapped onto Diffractor's.

#include "pch.h"
#include "files.h"
#include "libfiles/raw.h"

// libfiles hands LibRaw the native path: wide on Windows, bytes elsewhere, which is what
// platform::native_path already answers.
namespace
{
	auto libraw_path(const core::file_path path)
	{
		return platform::to_file_system_path(path);
	}

	// A surface as the memory libfiles writes a raw file's bitmap preview or thumbnail, or its developed
	// image, into. The raw data's declared size is judged against the decode budget before LibRaw develops
	// anything.
	class raw_surface_sink final : public libfiles::image_sink
	{
		const core::file_path _path;

	public:
		core::surface_ptr surface;
		// The size refused for the budget, read from the header.
		sizei over_budget;

		explicit raw_surface_sink(const core::file_path path) : _path(path)
		{
		}

		bool accept(const libfiles::frame_info& declared) override
		{
			const sizei dimensions{static_cast<int>(declared.source_width), static_cast<int>(declared.source_height)};
			if (dimensions.is_empty() || !files::exceeds_decode_budget(dimensions)) return true;

			core::log(__FUNCTION__, std::format("{} is {} x {}, past the decode budget of {}", _path.name(), dimensions.cx,
			                                  dimensions.cy, platform::format_size(core::file_size(render::max_decode_bytes))));
			over_budget = dimensions;
			return false;
		}

		libfiles::pixel_buffer allocate(const libfiles::frame_info& frame) override
		{
			auto s = std::make_shared<core::surface>();
			auto* const pixels = s->alloc(static_cast<int>(frame.width), static_cast<int>(frame.height),
			                              core::texture_format::RGB, static_cast<core::orientation>(frame.orientation));
			if (!pixels) return {};
			surface = std::move(s);
			return {pixels, surface->stride()};
		}
	};
}

file_load_result load_raw(const core::file_path path, const bool can_load_preview)
{
	file_load_result result;
	const std::filesystem::path native(libraw_path(path));
	raw_surface_sink sink(path);
	auto loaded = libfiles::load_raw(native, sink, can_load_preview);

	if (!loaded.jpeg.empty())
	{
		auto i = load_image_file({loaded.jpeg.data(), loaded.jpeg.size()});

		if (is_valid(i))
		{
			i->orientation(static_cast<core::orientation>(loaded.orientation));
			result.i = std::move(i);
			result.success = true;
			result.is_preview = true;
			return result;
		}

		// An image of some size that Diffractor makes nothing of: the raw data is developed, as it is for
		// any preview that does not load.
		loaded = libfiles::load_raw(native, sink, false);
	}

	if (loaded.success)
	{
		result.s = std::move(sink.surface);
		result.success = true;
		result.is_preview = loaded.preview;
	}
	else if (!sink.over_budget.is_empty())
	{
		// Named as a property of the file, like the other decoders name it, so it is not retried.
		result.source_dimensions = sink.over_budget;
		result.reason = file_load_result::failure::too_large;
	}
	else if (!loaded.decoded.error.empty())
	{
		core::log(__FUNCTION__, std::format("{}: {}", path.name(), loaded.decoded.error));
	}

	return result;
}

file_scan_result files::scan_raw(const core::file_path path, const std::string_view xmp_sidecar, const bool load_thumb,
                                 const sizei max, const libfiles::scan_intent intent)
{
	file_scan_result result;
	raw_surface_sink sink(path);
	auto scanned = libfiles::scan_raw(std::filesystem::path(libraw_path(path)), intent,
	                                  load_thumb ? &sink : nullptr);

	if (scanned.success)
	{
		result.width = scanned.width;
		result.height = scanned.height;
		result.orientation = static_cast<core::orientation>(scanned.orientation);
		result.metadata.xmp.assign(scanned.xmp.begin(), scanned.xmp.end());

		if (scanned.created)
		{
			const auto& c = *scanned.created;
			const core::date_t created(c.year, c.month, c.day, c.hour, c.minute, c.second);

			if (created.is_valid())
			{
				result.created_local = created;
				result.created_local_source = prop::date_source::embedded_created;
			}
		}

		result.pixel_format = core::cache(scanned.pixel_format);
		result.camera_manufacturer = core::cache(scanned.camera_make);
		result.camera_model = core::cache(scanned.camera_model);
		result.comment = core::cache(scanned.comment);
		result.artist = core::cache(scanned.artist);
		result.iso_speed = scanned.iso_speed;
		result.exposure_time = scanned.exposure_time;
		result.f_number = scanned.f_number;
		result.focal_length = scanned.focal_length;

		if (scanned.gps)
		{
			result.gps._latitude = scanned.gps->latitude;
			result.gps._longitude = scanned.gps->longitude;
		}

		if (intent == libfiles::scan_intent::inspect) result.libraw_metadata = to_metadata_kv(scanned.listing);
		result.success = true;
	}

	if (!scanned.thumbnail_jpeg.empty())
	{
		result.thumbnail_surface = image_to_surface(core::cspan{scanned.thumbnail_jpeg.data(), scanned.thumbnail_jpeg.size()},
		                                            max, false, decode_intent::thumbnail);

		if (result.thumbnail_surface)
		{
			// Decoded smaller than the preview's own size, as it always was, so the rule is judged on the
			// shape decoded.
			const auto decoded = result.thumbnail_surface->dimensions();
			result.thumbnail_surface->orientation(static_cast<core::orientation>(libfiles::raw_preview_orientation(
				decoded.cx, decoded.cy, static_cast<uint16_t>(result.thumbnail_surface->orientation()),
				scanned.image_width, scanned.image_height, scanned.image_orientation)));
		}
	}
	else if (scanned.thumbnail.success)
	{
		result.thumbnail_surface = std::move(sink.surface);
	}

	if (!core::is_empty(xmp_sidecar))
	{
		const auto sidecar = platform::blob_from_file(path.folder().combine_file(xmp_sidecar));
		result.metadata.xmp.assign(sidecar.begin(), sidecar.end());
	}

	return result;
}
