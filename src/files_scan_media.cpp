// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Audio and video scanning. libfiles reads the container - its streams, duration, tags, XMP
// packet and cover art - and its result is mapped here onto file_scan_result, the item's properties and
// the info panel, in Diffractor's own terms: short codec names, a pixel format as chroma and bit depth,
// and the audio sample type as coded. FFmpeg is opened only to decode, and to describe the streams of a
// file libfiles cannot describe whole.

#include "pch.h"
#include "files.h"
#include "av_format.h"
#include "metadata_xmp.h"
#include "libfiles/media.h"

namespace
{
	// The most cover art a scan reads into memory, as FFmpeg's attached picture was bounded by the file.
	constexpr uint64_t max_cover_art_bytes = 16ull * 1024ull * 1024ull;

	const libfiles::media_stream* first_stream(const libfiles::media_scan_result& scan,
	                                           const libfiles::media_stream_kind kind)
	{
		const auto found = std::ranges::find(scan.streams, kind, &libfiles::media_stream::kind);
		return found == scan.streams.end() ? nullptr : &*found;
	}

	// The front cover when there is one, else the first picture.
	const libfiles::media_picture* cover_picture(const libfiles::media_scan_result& scan)
	{
		if (scan.pictures.empty()) return nullptr;
		const auto front = std::ranges::find(scan.pictures, 3, &libfiles::media_picture::type);
		return front == scan.pictures.end() ? &scan.pictures.front() : &*front;
	}

	// The container's turn and mirror, numbered as EXIF numbers an orientation, as the player reads them from
	// FFmpeg's display matrix.
	ui::orientation orientation_of(const libfiles::video_format& v)
	{
		return static_cast<ui::orientation>(v.orientation());
	}

	// The height a non-square pixel displays at, as the FFmpeg path computes it for anamorphic video.
	uint32_t display_height(const libfiles::video_format& v)
	{
		const auto num = v.pixel_aspect_width;
		const auto den = v.pixel_aspect_height;
		if (num == 0 || den == 0 || num == den || v.width == 0 || v.height == 0) return v.height;
		return static_cast<uint32_t>(df::mul_div(static_cast<int64_t>(v.height), static_cast<int64_t>(den),
		                                         static_cast<int64_t>(num)));
	}

	df::date_t to_date(const libfiles::media_date& d)
	{
		if (!d.valid() || d.month == 0) return {};
		return {d.year, d.month, d.day == 0 ? 1 : d.day, d.hour, d.minute, d.second};
	}

	// A date as the pack records it: an instant, a wall-clock reading at a stated offset, or one in a
	// zone the file does not name.
	void add_date(prop::item_metadata& result, const prop::date_source source, const libfiles::media_date& d)
	{
		const auto date = to_date(d);
		if (!date.is_valid()) return;

		switch (d.zone)
		{
		case libfiles::media_date::time_zone::utc:
			result.dates.add_utc(source, date);
			break;
		case libfiles::media_date::time_zone::offset:
			result.dates.add(source, date, static_cast<int16_t>(d.offset_minutes));
			break;
		case libfiles::media_date::time_zone::unstated:
			result.dates.add(source, date);
			break;
		}
	}

	std::string tag_key(const libfiles::media_tag& tag)
	{
		if (tag.language.empty()) return tag.key;
		return std::format("{} ({})", tag.key, tag.language);
	}

	av_stream_type to_stream_type(const libfiles::media_stream_kind kind)
	{
		switch (kind)
		{
		case libfiles::media_stream_kind::video: return av_stream_type::video;
		case libfiles::media_stream_kind::audio: return av_stream_type::audio;
		case libfiles::media_stream_kind::subtitle: return av_stream_type::subtitle;
		case libfiles::media_stream_kind::data: return av_stream_type::data;
		}

		return av_stream_type::data;
	}

	// What libfiles says of a stream, over what the panel's row held.
	void describe_stream(const libfiles::media_stream& s, av_stream_info& row)
	{
		if (!s.codec.empty()) row.codec = s.codec;
		if (!s.codec_tag.empty()) row.fourcc = s.codec_tag;
		if (!s.title.empty()) row.title = s.title;
		if (!s.language.empty()) row.language = s.language;
		row.is_commentary = s.commentary;
		row.is_audio_description = s.audio_description;

		if (s.kind == libfiles::media_stream_kind::video)
		{
			row.pixel_format = s.video.pixel_format();
			row.profile = s.video.profile;
			row.bit_depth = s.video.bit_depth;
			row.hdr_transfer = std::string(s.video.hdr_transfer());
			row.rotation = s.video.rotation;
		}
		else if (s.kind == libfiles::media_stream_kind::audio)
		{
			row.audio_sample_rate = static_cast<int>(s.audio.sample_rate);
			row.audio_channels = static_cast<int>(s.audio.channels);
			row.audio_sample_type = coded_sample_type(s.audio.bits_per_sample, s.audio.float_samples);
			if (!s.audio.profile.empty()) row.profile = s.audio.profile;
		}
	}
}

prop::audio_sample_t coded_sample_type(const unsigned bits_per_sample, const bool float_samples)
{
	if (bits_per_sample == 0) return prop::audio_sample_t::none;
	if (float_samples) return bits_per_sample > 32 ? prop::audio_sample_t::signed_double : prop::audio_sample_t::signed_float;
	if (bits_per_sample <= 8) return prop::audio_sample_t::unsigned_8bit;
	if (bits_per_sample <= 16) return prop::audio_sample_t::signed_16bit;
	if (bits_per_sample <= 24) return prop::audio_sample_t::signed_24bit;
	if (bits_per_sample <= 32) return prop::audio_sample_t::signed_32bit;
	return prop::audio_sample_t::signed_64bit;
}

media_scan_read scan_media_file(const platform::file_ptr& f, const scan_intent intent, const bool want_cover_art)
{
	media_scan_read result;
	if (!f) return result;

	try
	{
		platform_file_stream stream(f);
		libfiles::reader r(stream);
		auto scan = std::make_shared<libfiles::media_scan_result>(libfiles::scan_media(
			r, intent == scan_intent::inspect ? libfiles::scan_intent::inspect : libfiles::scan_intent::index));

		// A format libfiles does not read is FFmpeg's to describe, which is no fault.
		if (!scan->error.empty() && scan->format != libfiles::media_format::unknown)
		{
			df::log(__FUNCTION__, scan->error);
		}

		if (want_cover_art && scan->success)
		{
			if (const auto* const picture = cover_picture(*scan))
			{
				if (!picture->data.empty())
				{
					result.cover_art.assign(picture->data.begin(), picture->data.end());
				}
				else if (!picture->location.empty() && picture->location.size <= max_cover_art_bytes &&
					r.contains(picture->location.offset, picture->location.size))
				{
					const auto bytes = r.read(picture->location.offset, static_cast<size_t>(picture->location.size));
					result.cover_art.assign(bytes.begin(), bytes.end());
				}
			}
		}

		result.scan = std::move(scan);
	}
	catch (const std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
	}

	// FFmpeg reads the handle from where it stands, so a decoder opened next must find it at the start.
	f->seek(0, platform::file::whence::begin);
	return result;
}

void apply_media_scan(file_scan_result& result, media_scan_read&& read, const bool stream_facts)
{
	if (!read.scan || !read.scan->success) return;

	const auto& scan = *read.scan;
	result.metadata.xmp.assign(scan.xmp.begin(), scan.xmp.end());
	if (!read.cover_art.empty()) result.cover_art = load_image_file(read.cover_art);

	if (stream_facts)
	{
		result.nb_streams = static_cast<uint32_t>(scan.streams.size());
		result.duration = df::round(scan.duration);
		if (scan.bit_rate > 0) result.bitrate = str::cache(prop::format_bit_rate(static_cast<int64_t>(scan.bit_rate)));

		if (const auto* const v = first_stream(scan, libfiles::media_stream_kind::video))
		{
			if (!v->codec.empty()) result.video_codec = str::cache(v->codec);
			result.width = v->video.width;
			result.height = display_height(v->video);
			if (const auto name = v->video.pixel_format(); !name.empty()) result.pixel_format = str::cache(name);
			result.orientation = orientation_of(v->video);
		}

		if (const auto* const a = first_stream(scan, libfiles::media_stream_kind::audio))
		{
			if (!a->codec.empty()) result.audio_codec = str::cache(a->codec);
			result.audio_sample_rate = static_cast<int>(a->audio.sample_rate);
			result.audio_channels = static_cast<int>(a->audio.channels);
			result.audio_sample_type = coded_sample_type(a->audio.bits_per_sample, a->audio.float_samples);
		}
	}

	result.media = std::move(read.scan);
}

void file_scan_result::parse_media_properties(prop::item_metadata& result) const
{
	const auto& p = media->properties;

	const auto set = [](str::cached& to, const std::string& value)
	{
		if (!value.empty()) to = str::strip_and_cache(value);
	};

	set(result.album, p.album);
	set(result.show, p.show);
	set(result.album_artist, p.album_artist);
	set(result.artist, p.artist);
	set(result.description, p.description);
	set(result.composer, p.composer);
	set(result.copyright_notice, p.copyright);
	set(result.encoder, p.encoder);
	set(result.genre, p.genre);
	set(result.publisher, p.publisher);
	set(result.synopsis, p.synopsis);
	set(result.title, p.title);
	set(result.camera_manufacturer, p.camera_make);
	set(result.camera_model, p.camera_model);
	set(result.performer, p.performer);

	// One comment is shown: an untagged one first, then one in the UI language, then any other.
	// Every comment stays in the info panel's tags.
	auto comment_priority = 0;

	for (const auto& c : p.comments)
	{
		const auto priority = c.language.empty() ? 3 : language_matches_ui(c.language, setting.language) ? 2 : 1;

		if (priority > comment_priority && !str::is_empty(c.text))
		{
			comment_priority = priority;
			result.comment = str::strip_and_cache(c.text);
		}
	}

	const auto pair = [](const int x, const int of)
	{
		return df::xy8(static_cast<uint8_t>(std::clamp(x, 0, 255)), static_cast<uint8_t>(std::clamp(of, 0, 255)));
	};

	if (p.year != 0) result.year = p.year;
	if (p.track != 0) result.track = pair(p.track, p.track_count);
	if (p.disc != 0) result.disk = pair(p.disc, p.disc_count);
	if (p.season != 0) result.season = p.season;
	if (p.episode != 0) result.episode = pair(p.episode, 0);
	if (p.rating) result.rating = std::clamp(*p.rating, 0, 5);

	add_date(result, prop::date_source::container_created, p.created);
	if (p.created.valid() && p.created.year > 1800 && p.created.year < 2100 && prop::is_null(result.year)) result.year = p.created.year;

	if (p.original_release.valid())
	{
		if (p.original_release.month == 0)
		{
			if (p.original_release.year > 1800 && p.original_release.year < 2100)
				result.dates.add(prop::date_source::id3_original_release, df::date_t(p.original_release.year, 1, 1, 0, 0, 0));
		}
		else
		{
			add_date(result, prop::date_source::id3_original_release, p.original_release);
		}
	}

	// A rip tag is a wall-clock reading: nobody wrote a zone for it.
	for (const auto& tag : media->tags)
	{
		if (str::icmp(tag.key, "TXXX:Rip date") == 0)
		{
			const auto date = df::date_t::from(tag.value);
			if (date.is_valid()) result.dates.add(prop::date_source::rip_date, date);
		}
	}

	for (const auto& k : p.keywords)
	{
		if (!k.empty()) keywords.emplace_back(str::cache(k));
	}

	if (p.location)
	{
		const gps_coordinate coordinate(p.location->latitude, p.location->longitude);
		if (coordinate.is_valid()) gps = coordinate;
	}
}

void add_media_blocks(const libfiles::media_scan_result& scan, av_media_info& info, const bool with_xmp)
{
	metadata_kv_list tags;

	for (const auto& tag : scan.tags)
	{
		tags.emplace_back(tag_key(tag), tag.value);
	}

	if (!tags.empty()) info.metadata.emplace_back(metadata_standard::media, std::move(tags));

	if (with_xmp && !scan.xmp.empty())
	{
		bool parsed = false;
		const df::cspan packet{scan.xmp.data(), scan.xmp.size()};
		auto kv = metadata_xmp::to_info(packet, parsed);

		constexpr size_t max_raw_bytes = 256 * 1024;
		std::string raw(std::bit_cast<const char*>(scan.xmp.data()), std::min(scan.xmp.size(), max_raw_bytes));
		info.metadata.emplace_back(metadata_standard::xmp, std::move(kv), scan.xmp.size(), parsed, std::move(raw));
	}

	if (!scan.structure.empty())
	{
		auto rows = scan.structure;
		info.metadata.emplace_back(metadata_standard::structure, to_metadata_kv(rows));
	}
}

void describe_media_streams(const libfiles::media_scan_result& scan, av_media_info& info)
{
	info.streams.clear();

	for (size_t i = 0; i < scan.streams.size(); ++i)
	{
		const auto& s = scan.streams[i];
		av_stream_info row;
		row.index = static_cast<int>(i);
		row.type = to_stream_type(s.kind);
		describe_stream(s, row);

		for (const auto& tag : s.tags)
		{
			row.metadata.emplace_back(tag_key(tag), tag.value);
		}

		info.streams.emplace_back(std::move(row));
	}

	const auto* const v = first_stream(scan, libfiles::media_stream_kind::video);
	const auto* const a = first_stream(scan, libfiles::media_stream_kind::audio);
	info.has_video = v != nullptr;
	info.has_audio = a != nullptr;
	info.has_multiple_audio_streams = std::ranges::count(scan.streams, libfiles::media_stream_kind::audio,
	                                                     &libfiles::media_stream::kind) > 1;
	info.bitrate = static_cast<int64_t>(scan.bit_rate);
	info.end = scan.duration;

	if (v)
	{
		info.video_codec = str::cache(v->codec);
		info.pixel_format = str::cache(v->video.pixel_format());
		info.video_frame_rate = v->video.frame_rate;
		info.render_dimensions = {static_cast<int>(v->video.width), static_cast<int>(v->video.height)};
		info.display_dimensions = {static_cast<int>(v->video.width), static_cast<int>(display_height(v->video))};
		info.display_orientation = orientation_of(v->video);
	}

	if (a)
	{
		info.audio_codec = str::cache(a->codec);
		info.audio_sample_rate = static_cast<int>(a->audio.sample_rate);
		info.audio_channels = static_cast<int>(a->audio.channels);
		info.audio_sample_type = coded_sample_type(a->audio.bits_per_sample, a->audio.float_samples);
	}
}

void overlay_media_scan(const libfiles::media_scan_result& scan, av_media_info& info, const std::vector<uint64_t>& stream_ids)
{
	if (!scan.success) return;

	// The rows stay FFmpeg's, in its order and with its indexes, since the track menu switches streams by
	// them; libfiles describes each row it can match. A row is matched by the container's stream number
	// where FFmpeg's demuxer records one, and otherwise - Matroska's leaves it zero - by its place among
	// the streams of its kind, which both read in the container's order.
	std::map<av_stream_type, int> ordinal;

	for (size_t i = 0; i < info.streams.size(); ++i)
	{
		auto& row = info.streams[i];
		const auto kind_index = ordinal[row.type]++;
		const auto id = i < stream_ids.size() ? stream_ids[i] : 0;
		const libfiles::media_stream* match = nullptr;

		if (id != 0)
		{
			const auto found = std::ranges::find_if(scan.streams, [&](const libfiles::media_stream& s)
			{
				return s.id == id && to_stream_type(s.kind) == row.type;
			});
			if (found != scan.streams.end()) match = &*found;
		}

		if (!match)
		{
			auto k = 0;

			for (const auto& s : scan.streams)
			{
				if (to_stream_type(s.kind) != row.type) continue;
				if (k++ == kind_index)
				{
					match = &s;
					break;
				}
			}
		}

		if (match) describe_stream(*match, row);
	}

	if (const auto* const v = first_stream(scan, libfiles::media_stream_kind::video))
	{
		if (!v->codec.empty()) info.video_codec = str::cache(v->codec);
		if (const auto name = v->video.pixel_format(); !name.empty()) info.pixel_format = str::cache(name);
	}

	if (const auto* const a = first_stream(scan, libfiles::media_stream_kind::audio))
	{
		if (!a->codec.empty()) info.audio_codec = str::cache(a->codec);
		info.audio_sample_type = coded_sample_type(a->audio.bits_per_sample, a->audio.float_samples);
	}

	// The container's own tags, its packet and its structure replace what FFmpeg's demuxer made of them.
	std::erase_if(info.metadata, [](const metadata_block& b)
	{
		return b.standard == metadata_standard::ffmpeg || b.standard == metadata_standard::media ||
			b.standard == metadata_standard::xmp || b.standard == metadata_standard::structure;
	});

	add_media_blocks(scan, info, true);
}
