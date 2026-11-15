// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Shared test fixture implementations -- scanning helpers, metadata comparison, index building, the shared gazetteer and the null av host used across every test file, a file turned as Rotate turns it, with the picture it then shows, and the ICC profiles a wide-gamut photo is tested with.

#include "pch.h"
#include "files.h"
#include "test.h"
#include "av_format.h"
#include "model_db.h"
#include "model_index.h"
#include "app_commands.h"
#include "test_fixtures.h"
#include "metadata.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Shared helper function definitions
///////////////////////////////////////////////////////////////////////////////////////////////////

file_scan_result ff_scan_file(files& ff, const core::file_path path, const std::string_view xmp_sidecar)
{
	const auto* const ft = files::file_type_from_name(path);
	return ff.scan_file(path, false, ft, xmp_sidecar, {}, libfiles::scan_intent::inspect);
}

rescan_spec ff_inspect_rescan(const core::file_path path)
{
	rescan_spec spec;
	spec.wanted = true;
	spec.file_type = files::file_type_from_name(path);
	spec.intent = libfiles::scan_intent::inspect;
	return spec;
}

file_scan_result ff_scan_after_update(files& ff, file_update_result& result, const core::file_path path,
                                      const std::string_view xmp_sidecar)
{
	if (result.scanned)
	{
		return std::move(result.scan);
	}

	const auto* const ft = files::file_type_from_name(path);
	return ff.scan_file(path, false, ft, xmp_sidecar, {}, libfiles::scan_intent::inspect);
}

file_scan_result ff_scan_and_load_thumb(files& ff, const core::file_path path, const std::string_view xmp_sidecar)
{
	const auto* const ft = files::file_type_from_name(path);
	return ff.scan_file(path, true, ft, xmp_sidecar, thumbnail_max_dimension);
}

file_update_result ff_turn(files& ff, const core::file_path path, const simple_transform t)
{
	file_scan_result info;
	{
		file_read_stream stream;
		stream.open(path);
		info = scan_photo(stream);
	}

	metadata_edits metadata;
	if (info.orientation != core::orientation::top_left) metadata.orientation = core::orientation::top_left;
	image_edits picture;
	picture.crop_bounds(rotate_command_crop_bounds(info.dimensions(), info.orientation, t, true));
	return ff.update(path, path, metadata, picture, file_encode_params{}, false, {});
}

core::const_surface_ptr ff_shown_picture(files& ff, const core::file_path path)
{
	const auto surface = ff.load(path, false).to_surface();
	if (!is_valid(surface) || surface->orientation() == core::orientation::top_left) return surface;
	return surface->transform(to_simple_transform(surface->orientation()));
}

double mean_channel_difference(const core::const_surface_ptr& a, const core::const_surface_ptr& b)
{
	const auto extent = a->dimensions();
	uint64_t difference = 0;

	for (auto y = 0; y < extent.cy; ++y)
	{
		for (auto x = 0; x < extent.cx; ++x)
		{
			const auto pa = a->get_pixel(x, y);
			const auto pb = b->get_pixel(x, y);
			for (auto shift = 0; shift < 24; shift += 8)
				difference += std::abs(static_cast<int>(pa >> shift & 0xFF) - static_cast<int>(pb >> shift & 0xFF));
		}
	}

	return static_cast<double>(difference) / (extent.cx * extent.cy * 3.0);
}

// Each tag's payload is a multiple of four bytes, so each starts where the specification asks.
std::vector<uint8_t> matrix_icc_profile(const std::array<double, 9>& colorants)
{
	const auto put = [](std::vector<uint8_t>& out, const size_t v)
	{
		for (auto shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
	};
	const auto fixed = [&put](std::vector<uint8_t>& out, const double v)
	{
		put(out, static_cast<uint32_t>(static_cast<int32_t>(std::lround(v * 65536.0))));
	};
	const auto xyz = [&put, &fixed](const double x, const double y, const double z)
	{
		std::vector<uint8_t> tag;
		put(tag, 0x58595A20); // 'XYZ '
		put(tag, 0);
		fixed(tag, x);
		fixed(tag, y);
		fixed(tag, z);
		return tag;
	};

	std::vector<uint8_t> curve;
	put(curve, 0x70617261); // 'para', its function 3: sRGB's
	put(curve, 0);
	put(curve, 0x00030000);
	for (const auto p : {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045}) fixed(curve, p);

	const auto& c = colorants;
	const std::pair<uint32_t, std::vector<uint8_t>> tags[] = {
		{0x77747074, xyz(0.9642, 1.0, 0.8249)}, // 'wtpt'
		{0x7258595A, xyz(c[0], c[1], c[2])},    // 'rXYZ'
		{0x6758595A, xyz(c[3], c[4], c[5])},    // 'gXYZ'
		{0x6258595A, xyz(c[6], c[7], c[8])},    // 'bXYZ'
		{0x72545243, curve},                    // 'rTRC'
		{0x67545243, curve},                    // 'gTRC'
		{0x62545243, curve},                    // 'bTRC'
	};

	std::vector<uint8_t> header;
	put(header, 0);
	put(header, 0);
	put(header, 0x04300000);
	put(header, 0x6D6E7472); // 'mntr'
	put(header, 0x52474220); // 'RGB '
	put(header, 0x58595A20); // 'XYZ '
	header.resize(36, 0);
	put(header, 0x61637370); // 'acsp'
	header.resize(68, 0);
	for (const auto v : {0.9642, 1.0, 0.8249}) fixed(header, v);
	header.resize(128, 0);

	auto profile = header;
	put(profile, std::size(tags));
	auto offset = profile.size() + std::size(tags) * 12;

	for (const auto& [signature, payload] : tags)
	{
		put(profile, signature);
		put(profile, offset);
		put(profile, payload.size());
		offset += payload.size();
	}

	for (const auto& tag : tags) profile.insert(profile.end(), tag.second.begin(), tag.second.end());

	std::vector<uint8_t> size;
	put(size, profile.size());
	std::copy(size.begin(), size.end(), profile.begin());
	return profile;
}

std::vector<uint8_t> display_p3_profile()
{
	return matrix_icc_profile({0.5151, 0.2412, -0.0011, 0.2920, 0.6922, 0.0419, 0.1571, 0.0666, 0.7841});
}

std::vector<uint8_t> srgb_profile()
{
	return matrix_icc_profile({0.4361, 0.2225, 0.0139, 0.3851, 0.7169, 0.0971, 0.1431, 0.0606, 0.7141});
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

prop::item_metadata_ptr metadata_from_cache(index_state& index, const core::file_path path)
{
	const auto node = index.validate_folder(path.folder(), true, core::utc_now());
	node.folder->is_in_collection = true;
	index.scan_item(node.folder, path, false, false, false, false, {}, false, files::file_type_from_name(path.name()));
	return index.find_item(path).metadata;
}

prop::item_metadata_ptr extract_properties(const core::file_path path, const uint32_t t)
{
	auto result = std::make_shared<prop::item_metadata>();
	const auto data = platform::blob_from_file(path);

	if (!data.empty())
	{
		mem_read_stream stream(data);

		if (stream.size() > 16)
		{
			const auto info = scan_photo(stream);

			// The blocks the flags name, reconciled as to_props reconciles them.
			libfiles::properties blocks;

			if (t & metadata_type::EXIF && !info.metadata.exif.empty())
			{
				libfiles::overlay(blocks, libfiles::properties_from_exif(libfiles::read_exif(info.metadata.exif)));
			}

			if (t & metadata_type::IPTC && !info.metadata.iptc.empty())
			{
				libfiles::overlay(blocks, libfiles::properties_from_iptc(libfiles::read_iptc(info.metadata.iptc)));
			}

			apply_properties(*result, blocks);

			if (t & metadata_type::XMP && !info.metadata.xmp.empty())
			{
				apply_properties(*result, libfiles::properties_from_xmp(info.metadata.xmp));
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
	result->dates.add(prop::date_source::exif_digitized, core::date_t(2012, 9, 14, 19, 21, 14));
	result->dates.add(prop::date_source::exif_original, core::date_t(2012, 9, 14, 19, 21, 14));
	result->dates.add(prop::date_source::exif_datetime, core::date_t(2012, 9, 14, 19, 21, 14));
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

df::index_file_item make_index_file_info(const core::date_t date)
{
	df::index_file_item result;
	result.file_modified = date;
	result.file_created = date;
	result.ft = files::file_type_from_name("test.jpg");
	result.safe_ps();
	return result;
}

df::item_element_ptr load_item(index_state& index, const core::file_path path, const bool load_thumb)
{
	auto i = std::make_shared<df::item_element>(path, index.find_item(path));
	index.scan_item(i, load_thumb, false);
	return i;
}

std::string_view detect_xmp_sidecar(const core::file_path path)
{
	const auto xmp_path = path.extension(".xmp");
	return platform::exists(xmp_path) ? xmp_path.name() : std::string_view{};
}

int count_search_results(index_state& index, const df::search_t& search)
{
	int result = 0;

	auto cb = [&result](const index_state::query_item_results& items, const bool /*completed*/)
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
	core::index_roots paths;
	paths.folders.emplace(test_files_folder());
	paths.excludes.emplace(test_files_folder().combine("excluded1"));
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
		const auto cache_path1 = test_temps.next_path();

		database db1(test_index);

		db1.open(cache_path1.folder(), cache_path1.file_name_without_extension());

		build_index(test_index, db1);

		const core::item_selector selector(test_files_folder(), true);
		df::item_set items1;

		auto cb1 = [this, &items1](index_state::query_item_results items, const bool /*completed*/)
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

		// Collection-wide searches are then answered by the search index while it is current, which holds
		// it to the counts each test was verified against when only the scan answered.
		test_index.update_search_index();
		assert_equal(true, test_index.search_index() != nullptr, "the shared index reads a search index");
		loaded = true;
	}
}

struct null_av_host final : av_host
{
	void invalidate_view(const view_invalid /*invalid*/) override {}

	void queue_ui(const std::function<void()> f) override { f(); }
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

std::shared_ptr<av_session> make_test_session(std::function<void(core::file_path, double)> save_position)
{
	static null_av_host navh;
	return std::make_shared<av_session>(navh, std::move(save_position));
}

std::shared_ptr<av_session> make_test_session_with_decode_wake(std::shared_ptr<const core::thread_event> decode_wake)
{
	static null_av_host navh;
	return std::make_shared<av_session>(navh, nullptr, std::move(decode_wake));
}
