// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Shared test fixture implementations -- scanning helpers, metadata comparison, index building, the shared gazetteer and the null av host used across every test file.

#include "pch.h"
#include "files.h"
#include "test.h"
#include "av_format.h"
#include "model_db.h"
#include "model_index.h"
#include "test_fixtures.h"
#include "metadata_exif.h"
#include "metadata_iptc.h"
#include "metadata_xmp.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Shared helper function definitions
///////////////////////////////////////////////////////////////////////////////////////////////////

file_scan_result ff_scan_file(files& ff, const df::file_path path, const std::string_view xmp_sidecar)
{
	const auto* const ft = files::file_type_from_name(path);
	return ff.scan_file(path, false, ft, xmp_sidecar, {}, scan_intent::inspect);
}

rescan_spec ff_inspect_rescan(const df::file_path path)
{
	rescan_spec spec;
	spec.wanted = true;
	spec.file_type = files::file_type_from_name(path);
	spec.intent = scan_intent::inspect;
	return spec;
}

file_scan_result ff_scan_after_update(files& ff, file_update_result& result, const df::file_path path,
                                      const std::string_view xmp_sidecar)
{
	if (result.scanned)
	{
		return std::move(result.scan);
	}

	const auto* const ft = files::file_type_from_name(path);
	return ff.scan_file(path, false, ft, xmp_sidecar, {}, scan_intent::inspect);
}

file_scan_result ff_scan_and_load_thumb(files& ff, const df::file_path path,
                                        const std::string_view xmp_sidecar)
{
	const auto* const ft = files::file_type_from_name(path);
	return ff.scan_file(path, true, ft, xmp_sidecar, thumbnail_max_dimension);
}

void assert_metadata(const prop::item_metadata& expected, const prop::item_metadata& actual,
                     const std::string_view message)
{
	assert_equal(expected.album, actual.album, "album", message);
	assert_equal(expected.album_artist, actual.album_artist, "album_artist", message);
	assert_equal(expected.artist, actual.artist, "artist", message);
	assert_equal(expected.audio_codec, actual.audio_codec, "audio_codec", message);
	assert_equal(expected.audio_sample_rate, actual.audio_sample_rate, "audio_sample_rate", message);
	assert_equal(expected.audio_sample_type, actual.audio_sample_type, "audio_sample_type", message);
	assert_equal(expected.audio_channels, actual.audio_channels, "audio_channels", message);
	assert_equal(expected.bitrate, actual.bitrate, "bitrate", message);
	assert_equal(expected.camera_manufacturer, actual.camera_manufacturer, "camera_manufacturer", message);
	assert_equal(expected.camera_model, actual.camera_model, "camera_model", message);
	assert_equal(expected.comment, actual.comment, "comment", message);
	assert_equal(expected.composer, actual.composer, "composer", message);
	assert_equal(expected.coordinate, actual.coordinate, "coordinate", message);
	assert_equal(expected.copyright_creator, actual.copyright_creator, "copyright_creator", message);
	assert_equal(expected.copyright_credit, actual.copyright_credit, "copyright_credit", message);
	assert_equal(expected.copyright_notice, actual.copyright_notice, "copyright_notice", message);
	assert_equal(expected.copyright_source, actual.copyright_source, "copyright_source", message);
	assert_equal(expected.copyright_url, actual.copyright_url, "copyright_url", message);
	assert_equal(expected.dates.original(), actual.dates.original(), "date original", message);
	assert_equal(expected.dates.created(), actual.dates.created(), "date created", message);
	// Modified is a date a user holds, so a fixture that stops at the other two cannot see a whole
	// third of the ladder change under it.
	assert_equal(expected.dates.modified(), actual.dates.modified(), "date modified", message);
	assert_equal(expected.description, actual.description, "description", message);
	assert_equal(expected.width, actual.width, "width", message);
	assert_equal(expected.height, actual.height, "height", message);
	assert_equal(expected.disk, actual.disk, "disk", message);
	assert_equal(expected.duration, actual.duration, "duration", message);
	assert_equal(expected.encoder, actual.encoder, "encoder", message);
	assert_equal(expected.episode, actual.episode, "episode", message);
	assert_equal(prop::format_exposure(expected.exposure_time), prop::format_exposure(actual.exposure_time),
	             "exposure_time", message);
	assert_equal(prop::format_f_num(expected.f_number), prop::format_f_num(actual.f_number), "f_number", message);
	assert_equal(expected.file_name, actual.file_name, "file_name", message);
	assert_equal(expected.focal_length, actual.focal_length, "focal_length", message);
	assert_equal(expected.focal_length_35mm_equivalent, actual.focal_length_35mm_equivalent,
	             "focal_length_35mm_equivalent", message);
	assert_equal(expected.genre, actual.genre, "genre", message);
	assert_equal(expected.iso_speed, actual.iso_speed, "iso_speed", message);
	assert_equal(expected.lens, actual.lens, "lens", message);
	assert_equal(expected.location_place, actual.location_place, "location_city", message);
	assert_equal(expected.location_country, actual.location_country, "location_country", message);
	assert_equal(expected.location_state, actual.location_state, "location_state", message);
	assert_equal(expected.orientation, actual.orientation, "orientation", message);
	assert_equal(expected.performer, actual.performer, "performer", message);
	assert_equal(expected.publisher, actual.publisher, "publisher", message);
	assert_equal(expected.rating, actual.rating, "rating", message);
	assert_equal(expected.season, actual.season, "season", message);
	assert_equal(expected.show, actual.show, "show", message);
	assert_equal(expected.synopsis, actual.synopsis, "synopsis", message);
	assert_equal(expected.tags, actual.tags, "tags", message);
	assert_equal(expected.title, actual.title, "title", message);
	assert_equal(expected.track, actual.track, "track", message);
	assert_equal(expected.video_codec, actual.video_codec, "video_codec", message);
	assert_equal(expected.year, actual.year, "year", message);
}

prop::item_metadata_ptr metadata_from_cache(index_state& index, const df::file_path path)
{
	const auto node = index.validate_folder(path.folder(), true, platform::now());
	node.folder->is_in_collection = true;
	index.scan_item(node.folder, path, false, false, false, false, {}, false,
	                files::file_type_from_name(path.name()));
	return index.find_item(path).metadata;
}


prop::item_metadata_ptr extract_properties(const df::file_path path, const uint32_t t)
{
	auto result = std::make_shared<prop::item_metadata>();
	const auto data = blob_from_file(path);

	if (!data.empty())
	{
		mem_read_stream stream(data);

		if (stream.size() > 16)
		{
			const auto info = scan_photo(stream);

			if (t & metadata_type::EXIF && !info.metadata.exif.empty())
			{
				metadata_exif::parse(*result, info.metadata.exif);
			}

			if (t & metadata_type::IPTC && !info.metadata.iptc.empty())
			{
				metadata_iptc::parse(*result, info.metadata.iptc);
			}

			if (t & metadata_type::XMP && !info.metadata.xmp.empty())
			{
				metadata_xmp::parse(*result, info.metadata.xmp);
			}

			if (t == metadata_type::ALL)
			{
				result = info.to_props();
			}
		}
	}

	return result;
}


prop::item_metadata_ptr expected_test_jpg()
{
	auto result = std::make_shared<prop::item_metadata>();
	result->f_number = 6.3f;
	result->focal_length = 15.0f;
	result->camera_manufacturer = "Canon"_c;
	result->camera_model = "Canon EOS 7D"_c;
	result->coordinate = gps_coordinate(50.08806, 14.42083);
	result->copyright_notice = "Copyright"_c;
	result->dates.add(prop::date_source::exif_digitized, df::date_t(2012, 9, 14, 19, 21, 14));
	result->dates.add(prop::date_source::exif_original, df::date_t(2012, 9, 14, 19, 21, 14));
	result->dates.add(prop::date_source::exif_datetime, df::date_t(2012, 9, 14, 19, 21, 14));
	result->description = "Caption"_c;
	result->exposure_time = 1.0f / 100.0f;
	result->iso_speed = 100;
	result->lens = "EF-S15-85mm f/3.5-5.6 IS USM"_c;
	result->location_place = "Prague"_c;
	result->location_country = "Czech Republic"_c;
	result->location_state = "Hlavní Mesto Praha"_c;
	result->rating = 4;
	result->tags = "key1 key2 key3"_c;
	result->title = "Title"_c;
	result->width = 1024;
	result->height = 683;
	result->pixel_format = "YCbCr"_c;

	return result;
}


df::index_file_item make_index_file_info(const df::date_t date)
{
	df::index_file_item result;
	result.file_modified = date;
	result.file_created = date;
	result.ft = files::file_type_from_name("test.jpg");
	result.safe_ps();
	return result;
}


df::item_element_ptr load_item(index_state& index, const df::file_path path, const bool load_thumb)
{
	auto i = std::make_shared<df::item_element>(path, index.find_item(path));
	index.scan_item(i, load_thumb, false);
	return i;
}

std::string_view detect_xmp_sidecar(const df::file_path path)
{
	const auto xmp_path = path.extension(".xmp");
	return xmp_path.exists() ? xmp_path.name() : std::string_view{};
}

int count_search_results(index_state& index, const df::search_t& search)
{
	int result = 0;

	auto cb = [&result](const index_state::query_item_results& items, const bool completed)
	{
		result += static_cast<int>(items.size());
	};

	index.query_items(search, cb, test_token);
	return result;
}

int count_search_results(index_state& index, const std::string_view query)
{
	return count_search_results(index, df::search_t::parse(query));
}

void build_index(index_state& index, database& db)
{
	df::index_roots paths;
	paths.folders.emplace(test_files_folder);
	paths.excludes.emplace(test_files_folder.combine("excluded1"));
	paths.exclude_wildcards.emplace("exclud*2"_c);

	index.index_roots(paths);
	index.index_folders(test_token);
	index.scan_uncached(test_token);
	db.perform_writes();

	assert_equal(expected_cached_item_count, index.stats.media_item_count, "cached item count");

	// The sidebar gates its per-item counts and the item totals gate their text on this, so a built
	// index that never reports itself complete would leave both stuck on the loading affordance.
	assert_equal(true, index.is_init_complete(), "index reports init complete", "cached item count");
}


void shared_test_context::lazy_load_index()
{
	if (!loaded)
	{
		const auto cache_path1 = _temps.next_path();

		database db1(test_index);

		db1.open(cache_path1.folder(), cache_path1.file_name_without_extension());

		build_index(test_index, db1);

		const df::item_selector selector(test_files_folder, true);
		df::item_set items1;

		auto cb1 = [this, &items1](index_state::query_item_results items, const bool completed)
		{
			items1.append(test_index.materialize_query_items(std::move(items), {}));
		};

		test_index.query_items(df::search_t().add_selector(selector), cb1, test_token);

		items1.for_all([](const auto& item) { item->begin_db_thumbnail_query(); });
		db1.load_thumbnails(test_index, database::make_thumbnail_requests(items1));
		test_index.scan_items(items1, false, false, false, false, test_token);
		db1.perform_writes();

		assert_equal(expected_cached_item_count, test_index.stats.media_item_count, "cached item count");
		assert_equal(0, empty_index.stats.media_item_count, "cached item count");
		loaded = true;
	}
}

struct null_av_host final : av_host
{
	void invalidate_view(const view_invalid invalid) override
	{
	}

	void queue_ui(const std::function<void()> f) override
	{
		f();
	}
};

location_cache& test_locations()
{
	static location_cache cache;
	if (!cache.is_index_loaded()) cache.load_index();
	return cache;
}

std::shared_ptr<av_player> make_test_player()
{
	static null_av_host navh;
	return std::make_shared<av_player>(navh);
}

std::shared_ptr<av_session> make_test_session()
{
	return make_test_session(nullptr);
}

std::shared_ptr<av_session> make_test_session(std::function<void(df::file_path, double)> save_position)
{
	static null_av_host navh;
	return std::make_shared<av_session>(navh, std::move(save_position));
}
