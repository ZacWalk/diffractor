// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Commodore 64 disk, tape, cartridge and program images: the listing libfiles makes of one, drawn
// by libfiles into a surface for the disk control's texture. And SID tunes, whose header lib8bit reads.

#include "pch.h"
#include "files.h"
#include "av_sid.h"

#include "machine.h"

namespace
{
	// "1987 System 3" and "1987-88 Ocean": a release field usually leads with its year, and the publisher
	// follows. Anything else - "198? Unknown", "(C) Hewson" - is all publisher.
	std::pair<int, std::string_view> split_sid_release(const std::string_view released)
	{
		if (released.size() >= 4 && str::is_num(released.substr(0, 4)))
		{
			const auto year = str::to_int(released.substr(0, 4));

			if (year >= 1980 && year < 2100)
			{
				auto rest = released.substr(4);
				const auto range_end = rest.find_first_not_of("-0123456789");
				rest = range_end == std::string_view::npos ? std::string_view{} : rest.substr(range_end);
				return {year, str::trim(rest)};
			}
		}

		return {0, str::trim(released)};
	}

	std::string sid_address(const uint16_t address)
	{
		return std::format("${:04X}", address);
	}
}

ui::const_surface_ptr files::c64_listing_surface(const libfiles::c64_listing& listing, const uint32_t fg,
                                                 const uint32_t bg)
{
	if (listing.rows.empty() || listing.columns == 0) return {};

	auto surface = std::make_shared<ui::surface>();
	auto* const pixels = surface->alloc(static_cast<int>(listing.width()), static_cast<int>(listing.height()),
	                                    ui::texture_format::RGB);
	if (!pixels) return {};

	// RGB textures upload as B8G8R8X8, the order libfiles draws in, given its colours as 0xRRGGBB. A
	// listing is a whole number of eight-pixel cells wide, so its rows fill their stride, and no padding
	// goes to the GPU unwritten.
	libfiles::draw_c64_listing(listing, {pixels, surface->stride()}, ui::bgr(fg), ui::bgr(bg));
	return surface;
}

file_scan_result files::scan_sid(const df::cspan data)
{
	file_scan_result result;
	sid_tune_info info;
	if (data.empty() || !machine::parse_sid(data.data, data.size, info)) return result;

	const auto texts = read_sid_header_texts(data);
	const auto& title = texts.title;
	const auto& author = texts.author;
	const auto& released = texts.released;
	const auto [year, publisher] = split_sid_release(released);
	const auto chip = sid_chip_name(info.chip_8580);

	// Named as FFmpeg names a container's tags, so the properties are read the way every other audio
	// file's are. The release field was once the copyright field, and its year and publisher are what a
	// collection is browsed by. The track is the tune the file starts on, out of the tunes it holds.
	auto& kv = result.ffmpeg_metadata;
	if (!title.empty()) kv.emplace_back("title", title);
	if (!author.empty()) kv.emplace_back("artist", author);
	if (!released.empty()) kv.emplace_back("copyright", released);
	if (year) kv.emplace_back("year", std::to_string(year));
	if (!publisher.empty()) kv.emplace_back("publisher", std::string(publisher));
	kv.emplace_back("track", std::format("{}/{}", info.start_song, info.songs));
	kv.emplace_back("system", "Commodore 64");

	result.duration = sid_tune_seconds;
	result.audio_codec = str::cache(chip);
	result.audio_sample_rate = sid_sample_rate;
	result.audio_channels = 1;
	result.audio_sample_type = prop::audio_sample_t::signed_16bit;
	result.nb_streams = 1;

	auto& rows = result.structure_metadata;
	add_structure_section(rows, "SID tune", "sid.summary", true);
	add_structure_row(rows, "Format", std::format("{} version {}", info.rsid ? "RSID" : "PSID", info.version));
	add_structure_row(rows, "Tunes", std::format("{}, starting with {}", info.songs, info.start_song));
	add_structure_row(rows, "Chip", std::string(chip));
	add_structure_row(rows, "Clock", info.ntsc ? "NTSC" : "PAL");
	add_structure_row(rows, "Load address", sid_address(info.load_address));
	add_structure_row(rows, "Init address", sid_address(info.init_address));
	add_structure_row(rows, "Play address",
	                  info.play_address ? sid_address(info.play_address) : "set by the tune's own interrupt");
	finish_structure_sections(rows);

	result.success = true;
	return result;
}
