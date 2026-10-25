// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Property definitions and metadata types. Defines all displayable and searchable
// properties like camera, date, dimensions, rating, and their formatting.

#include "pch.h"
#include "app_icons.h"
#include "model_location.h"
#include "model_items.h"

df_assert_movable(prop::item_metadata);

using property_map = df::hash_map<std::string_view, prop::key_ref, df::ihash, df::ieq>;
static property_map all_props;


int64_t prop::exp_round(const double d)
{
	const auto n = df::round64(d);
	if (d >= 950ull) return df::round64(n, 1000) * 1000;
	if (d >= 95ull) return df::round64(n, 100) * 100;
	if (d >= 5ull) return df::round64(n, 10) * 10;
	return n;
}

df::xy32 df::xy32::null = {0, 0};

df::xy32 df::xy32::parse(const std::string_view r)
{
	if (str::is_num(r))
	{
		return make(str::to_int(r), 0);
	}

	int x, y;

	if (_snscanf_s(std::bit_cast<const char*>(r.data()), r.size(), "%d/%d", &x, &y) == 2)
	{
		return make(x, y);
	}

	return null;
}

df::xy8 df::xy8::parse(const std::string_view r)
{
	if (str::is_num(r))
	{
		return make(str::to_int(r), 0);
	}

	int x, y;

	if (_snscanf_s(std::bit_cast<const char*>(r.data()), r.size(), "%d/%d", &x, &y) == 2)
	{
		return make(x, y);
	}

	return {0, 0};
}

prop::key::key(const uint16_t id, std::string_view sn, const std::string_view n, text_t& tx,
               const icon_index i, const prop::data_type t, const uint32_t f, const uint32_t bit) : id(id), icon(i),
	name(str::cache(n)), text_key(tx), data_type(t), flags(f), search_presence_bit(bit)
{
	df::assert_true(!all_props.contains(name));
	all_props[name] = this;
}

std::string prop::key::text() const
{
	return tt.translate_text(std::string(text_key.sv()), "property");
}

static text_t prop_name_crc32c = "crc32c"sv;
// Never displayed and never typed by a user - `@panorama` is the spelling they see - so it carries
// a local name rather than a catalog entry in thirteen languages.
static text_t prop_name_panorama = "panorama"sv;

prop::key prop::album('al', "", "album", tt.prop_name_album, icon_index::star, data_type::string,
                      style::groupable | style::sortable | style::auto_complete, search_presence_mask::album);
prop::key prop::show('sw', "", "show", tt.prop_name_show, icon_index::star, data_type::string,
                     style::groupable | style::sortable | style::auto_complete, 0);
prop::key prop::season('ss', "", "season", tt.prop_name_season, icon_index::star, data_type::int32,
                       style::groupable | style::sortable, 0);
prop::key prop::episode('ep', "", "episode", tt.prop_name_episode, icon_index::star, data_type::int_pair,
                        style::groupable | style::sortable, 0);
prop::key prop::artist('ar', "", "artist", tt.prop_name_artist, icon_index::person, data_type::string,
                       style::groupable | style::sortable | style::auto_complete, search_presence_mask::artist);
prop::key prop::album_artist('aa', "", "album.artist", tt.prop_name_albumartist, icon_index::person,
                             data_type::string, style::groupable | style::sortable | style::auto_complete,
                             search_presence_mask::artist);
prop::key prop::audio_codec('ac', "", "audio.codec", tt.prop_name_audiocodec, icon_index::audio,
                            data_type::string,
                            style::groupable | style::sortable | style::auto_complete, search_presence_mask::codec);
prop::key prop::bitrate('br', "", "bitrate", tt.prop_name_bitrate, icon_index::star, data_type::string,
                        style::fuzzy_search | style::groupable | style::sortable | style::auto_complete, 0);
prop::key prop::camera_manufacturer('ca', "", "camera.manufacturer", tt.prop_name_cameramanufacturer,
                                    icon_index::camera, data_type::string,
                                    style::groupable | style::sortable | style::auto_complete,
                                    search_presence_mask::camera);
prop::key prop::camera_model('cm', "", "camera", tt.prop_name_camera, icon_index::camera, data_type::string,
                             style::groupable | style::sortable | style::auto_complete, search_presence_mask::camera);
prop::key prop::audio_channels('ch', "", "audio.channels", tt.prop_name_channels, icon_index::star,
                               data_type::int32, style::sortable, search_presence_mask::audio_codec);
prop::key prop::audio_sample_rate('sr', "", "audio.sample.rate", tt.prop_name_samplerate, icon_index::star,
                                  data_type::int32, style::groupable | style::sortable | style::auto_complete,
                                  search_presence_mask::audio_codec);
prop::key prop::audio_sample_rate_full('sR', "", "audio.sample.rate.full", tt.prop_name_samplerate, icon_index::star,
                                       data_type::int32, style::none, search_presence_mask::audio_codec);
prop::key prop::audio_sample_type('sa', "", "audio.sample.type", tt.prop_name_sampletype, icon_index::star,
                                  data_type::int32, style::groupable | style::sortable | style::auto_complete,
                                  search_presence_mask::audio_codec);
prop::key prop::location_place('ci', "", "place", tt.prop_name_place, icon_index::world, data_type::string,
                               style::groupable | style::sortable | style::auto_complete,
                               search_presence_mask::location);
prop::key prop::comment('ct', "", "comment", tt.prop_name_comment, icon_index::star, data_type::string2,
                        style::none, search_presence_mask::text);
prop::key prop::description('de', "", "description", tt.prop_name_description, icon_index::star,
                            data_type::string2,
                            style::none, search_presence_mask::text);
prop::key prop::composer('co', "", "composer", tt.prop_name_composer, icon_index::star, data_type::string,
                         style::sortable, search_presence_mask::artist);
prop::key prop::copyright_credit('cd', "", "copyright.credit", tt.prop_name_copyrightcredit,
                                 icon_index::copyright,
                                 data_type::string, style::groupable | style::sortable | style::auto_complete,
                                 search_presence_mask::credit);
prop::key prop::copyright_source('cs', "", "copyright.source", tt.prop_name_copyrightsource,
                                 icon_index::copyright,
                                 data_type::string, style::groupable | style::sortable | style::auto_complete,
                                 search_presence_mask::credit);
prop::key prop::copyright_creator('cc', "", "copyright.creator", tt.prop_name_copyrightcreator,
                                  icon_index::copyright, data_type::string,
                                  style::groupable | style::sortable | style::auto_complete,
                                  search_presence_mask::credit);
prop::key prop::copyright_notice('cp', "", "copyright.notice", tt.prop_name_copyrightnotice,
                                 icon_index::copyright,
                                 data_type::string, style::groupable | style::sortable | style::auto_complete,
                                 search_presence_mask::credit);
prop::key prop::copyright_url('cw', "", "copyright.url", tt.prop_name_copyrighturl, icon_index::copyright,
                              data_type::string, style::groupable | style::sortable | style::auto_complete,
                              search_presence_mask::credit);
prop::key prop::location_country('cn', "", "country", tt.prop_name_country, icon_index::world,
                                 data_type::string,
                                 style::groupable | style::sortable | style::auto_complete,
                                 search_presence_mask::location);
prop::key prop::created_exif('c', "", "original", tt.prop_name_original, icon_index::time,
                             data_type::date,
                             style::groupable | style::sortable | style::auto_complete, search_presence_mask::created);
// Registered only so a row written before the date pack can still be read; "digitized" as a search
// word is aliased to the created date, which is the concept it merged into.
prop::key prop::created_digitized('cz', "", "digitized", tt.prop_name_digitized, icon_index::time,
                                  data_type::date, style::none, search_presence_mask::created);
prop::key prop::created_utc('cu', "", "created", tt.prop_name_created, icon_index::time, data_type::date,
                            style::groupable | style::sortable | style::auto_complete, search_presence_mask::created);
prop::key prop::dates_packed('dp', "", "date.pack", tt.prop_name_created, icon_index::time, data_type::date,
                             style::none, search_presence_mask::created);
prop::key prop::disk_num('dk', "", "disk", tt.prop_name_disk, icon_index::star, data_type::int_pair,
                         style::groupable | style::sortable, 0);
prop::key prop::dimensions('di', "", "dimensions", tt.prop_name_dimensions, icon_index::star,
                           data_type::int_pair,
                           style::groupable | style::sortable, 0);
prop::key prop::duration('du', "", "duration", tt.prop_name_duration, icon_index::time, data_type::int32,
                         style::fuzzy_search | style::groupable | style::sortable | style::auto_complete,
                         search_presence_mask::duration);
prop::key prop::encoder('en', "", "encoder", tt.prop_name_encoder, icon_index::star, data_type::string,
                        style::groupable | style::sortable | style::auto_complete, 0);
prop::key prop::encoding_tool('es', "", "encoding.tool", tt.prop_name_encodingtool, icon_index::star,
                              data_type::string, style::groupable | style::sortable | style::auto_complete, 0);
prop::key prop::exposure_time('et', "", "exposure", tt.prop_name_exposure, icon_index::camera,
                              data_type::float32,
                              style::fuzzy_search | style::groupable | style::sortable | style::auto_complete,
                              search_presence_mask::camera_settings);
prop::key prop::f_number('fs', "", "fnumber", tt.prop_name_fnumber, icon_index::star, data_type::float32,
                         style::groupable | style::sortable | style::auto_complete,
                         search_presence_mask::camera_settings);
prop::key prop::focal_length('fl', "", "focal.length", tt.prop_name_focallength, icon_index::star,
                             data_type::float32,
                             style::fuzzy_search | style::groupable | style::sortable | style::auto_complete,
                             search_presence_mask::camera_settings);
prop::key prop::focal_length_35mm_equivalent('f3', "", "focal.length.35mm.equivalent",
                                             tt.prop_name_35mmequivalent,
                                             icon_index::star, data_type::int32, style::none,
                                             search_presence_mask::camera_settings);
prop::key prop::pixel_format('pf', "", "pixel.format", tt.prop_name_pixelformat, icon_index::star,
                             data_type::string, style::groupable | style::sortable | style::auto_complete, 0);
prop::key prop::genre('gn', "", "genre", tt.prop_name_genre, icon_index::star, data_type::string,
                      style::groupable | style::sortable | style::auto_complete, search_presence_mask::genre);
prop::key prop::iso_speed('is', "", "iso", tt.prop_name_iso, icon_index::star, data_type::int32,
                          style::groupable | style::sortable | style::auto_complete,
                          search_presence_mask::camera_settings);
prop::key prop::latitude('lx', "", "latitude", tt.prop_name_latitude, icon_index::world, data_type::float32,
                         style::none, search_presence_mask::location);
// locations.md 2.8: metres above sea level and km/h. Neither is groupable or sortable -- they
// exist to classify height, and a pile of distinct altitudes answers nothing.
prop::key prop::altitude('la', "", "altitude", tt.prop_name_altitude, icon_index::world, data_type::float32,
                         style::none, search_presence_mask::location);
prop::key prop::gps_speed('lv', "", "gps.speed", tt.prop_name_gps_speed, icon_index::world, data_type::float32,
                          style::none, search_presence_mask::location);
prop::key prop::lens('lm', "", "lens", tt.prop_name_lens, icon_index::camera, data_type::string,
                     style::groupable | style::sortable | style::auto_complete, search_presence_mask::camera);
prop::key prop::longitude('ly', "", "longitude", tt.prop_name_longitude, icon_index::world, data_type::float32,
                          style::none, search_presence_mask::location);
prop::key prop::media_category('mc', "", "media.category", tt.prop_name_mediacategory, icon_index::star,
                               data_type::int32, style::groupable | style::sortable | style::auto_complete, 0);
prop::key prop::megapixels('mp', "", "megapixels", tt.prop_name_megapixels, icon_index::star,
                           data_type::float32,
                           style::fuzzy_search | style::groupable | style::sortable | style::auto_complete, 0);
prop::key prop::modified('m', "", "modified", tt.prop_name_modified, icon_index::time, data_type::date,
                         style::groupable | style::sortable | style::auto_complete, 0);
prop::key prop::null(0, "", "null", tt.prop_name_null, icon_index::star, data_type::int32, style::none, 0);
prop::key prop::orientation('or', "", "orientation", tt.prop_name_orientation, icon_index::star,
                            data_type::int32,
                            style::none, search_presence_mask::camera_settings);
prop::key prop::publisher('pb', "", "publisher", tt.prop_name_publisher, icon_index::star, data_type::string,
                          style::groupable | style::sortable | style::auto_complete, search_presence_mask::artist);
prop::key prop::performer('pm', "", "performer", tt.prop_name_performer, icon_index::star, data_type::string,
                          style::groupable | style::sortable | style::auto_complete, search_presence_mask::artist);
prop::key prop::rating('rt', "", "rating", tt.prop_name_rating, icon_index::star, data_type::int32,
                       style::groupable | style::sortable | style::auto_complete,
                       search_presence_mask::rating_label);
prop::key prop::file_size('s', "", "size", tt.prop_name_size, icon_index::star, data_type::size,
                          style::fuzzy_search | style::groupable | style::sortable | style::auto_complete, 0);
prop::key prop::location_state('st', "", "state", tt.prop_name_state, icon_index::world, data_type::string,
                               style::groupable | style::sortable | style::auto_complete,
                               search_presence_mask::location);
prop::key prop::streams('sm', "", "streams", tt.prop_name_streams, icon_index::star, data_type::int32,
                        style::groupable | style::sortable, 0);
prop::key prop::synopsis('sy', "", "synopsis", tt.prop_name_synopsis, icon_index::star, data_type::string,
                         style::none, search_presence_mask::text);
prop::key prop::tag('tg', "", "tag", tt.prop_name_tag, icon_index::tag, data_type::string,
                    style::groupable | style::multi_value | style::auto_complete, search_presence_mask::tag);
prop::key prop::title('tt', "", "title", tt.prop_name_title, icon_index::star, data_type::string,
                      style::sortable,
                      search_presence_mask::text);
prop::key prop::track_num('tr', "", "track", tt.prop_name_track, icon_index::star, data_type::int_pair,
                          style::sortable, 0);
prop::key prop::video_codec('vc', "", "video.codec", tt.prop_name_videocodec, icon_index::video,
                            data_type::string,
                            style::groupable | style::sortable | style::auto_complete, search_presence_mask::codec);
prop::key prop::year('yr', "", "year", tt.prop_name_year, icon_index::time, data_type::int32,
                     style::groupable | style::sortable | style::auto_complete, search_presence_mask::year);
prop::key prop::unique_id('id', "", "id", tt.prop_name_id, icon_index::star, data_type::string2, style::none,
                          0);
prop::key prop::file_name('fn', "", "file.name", tt.prop_name_filename, icon_index::star, data_type::string,
                          style::none, 0);
prop::key prop::raw_file_name('rf', "", "raw.file", tt.prop_name_rawfile, icon_index::star, data_type::string,
                              style::none, 0);
prop::key prop::system('se', "", "system", tt.prop_name_system, icon_index::star, data_type::string,
                       style::none,
                       search_presence_mask::game);
prop::key prop::game('gm', "", "game", tt.prop_name_game, icon_index::star, data_type::string, style::none,
                     search_presence_mask::game);

prop::key prop::crc32c('cr', "", "crc32c", prop_name_crc32c, icon_index::star, data_type::int32, style::none,
                       search_presence_mask::crc32c);

prop::key prop::label('lb', "", "label", tt.prop_name_label, icon_index::flag, data_type::string,
                      style::groupable | style::sortable | style::auto_complete,
                      search_presence_mask::rating_label);
prop::key prop::doc_id('ii', "", "document.id", tt.prop_name_doc_id, icon_index::star, data_type::int32,
                       style::none, search_presence_mask::doc_id);
prop::key prop::panorama('pj', "", "panorama.projection", prop_name_panorama, icon_index::world,
                         data_type::int32, style::none, search_presence_mask::panorama);


static df::hash_map<unsigned short, prop::key_ref> build_properties_by_id()
{
	df::hash_map<unsigned short, prop::key_ref> result;

	for (const auto& i : all_props)
	{
		const auto& prop = i.second;
		df::assert_true(!result.contains(prop->id));
		result[prop->id] = prop;
	}

	return result;
}


static property_map build_properties_by_name()
{
	property_map result;

	for (const auto& i : all_props)
	{
		const auto& prop = i.second;
		result[prop->name] = prop;
	}

	result["albums"] = prop::album;
	result["genres"] = prop::genre;
	result["lenses"] = prop::lens;
	result["aperture"] = prop::f_number;
	result["camera"] = prop::camera_model;
	result["cameraManufacturer"] = prop::camera_manufacturer;
	result["cameraModel"] = prop::camera_model;
	result["cameramake"] = prop::camera_manufacturer;
	result["cameramodel"] = prop::camera_model;
	result["cameras"] = prop::camera_model;
	result["caption"] = prop::description;
	result["city"] = prop::location_place;
	result["changed"] = prop::modified;
	result["copyright"] = prop::copyright_notice;
	result["creator"] = prop::copyright_creator;
	result["source"] = prop::copyright_source;
	result["countries"] = prop::location_country;
	result["country"] = prop::location_country;
	result["created"] = prop::created_utc;
	result["created.exif"] = prop::created_exif;
	result["digitized"] = prop::created_utc;
	result["original"] = prop::created_exif;
	result["credit"] = prop::copyright_credit;
	result["credits"] = prop::copyright_credit;
	result["comm"] = prop::comment;
	result["desc"] = prop::description;
	result["dimensions"] = prop::dimensions;
	result["exposureTime"] = prop::exposure_time;
	result["exposure"] = prop::exposure_time;
	result["film"] = prop::focal_length_35mm_equivalent;
	result["filmEquivalent"] = prop::focal_length_35mm_equivalent;
	result["fspot"] = prop::f_number;
	result["fnumber"] = prop::f_number;
	result["focalLength"] = prop::focal_length;
	result["focalLength35mmEquivalent"] = prop::focal_length_35mm_equivalent;
	result["iso"] = prop::iso_speed;
	result["isospeed"] = prop::iso_speed;
	result["latitude"] = prop::latitude;
	result["lens"] = prop::lens;
	result["longitude"] = prop::longitude;
	result["make"] = prop::camera_manufacturer;
	result["megapixels"] = prop::megapixels;
	result["pixels"] = prop::megapixels;
	result["megapixel"] = prop::megapixels;
	result["model"] = prop::camera_model;
	result["modified"] = prop::modified;
	result["mp"] = prop::megapixels;
	result["orientation"] = prop::orientation;
	result["rating"] = prop::rating;
	result["ratings"] = prop::rating;
	result["size"] = prop::file_size;
	result["filesize"] = prop::file_size;
	result["speed"] = prop::iso_speed;
	result["star"] = prop::rating;
	result["stars"] = prop::rating;
	result["state"] = prop::location_state;
	result["programme"] = prop::show;
	result["program"] = prop::show;
	result["tag"] = prop::tag;
	result["tagged"] = prop::tag;
	result["tags"] = prop::tag;
	result["taken"] = prop::created_exif;
	result["timeline"] = prop::created_exif;
	result["updated"] = prop::modified;
	result["when"] = prop::created_exif;
	result["x"] = prop::latitude;
	result["years"] = prop::year;
	result["year"] = prop::year;

	// single-letter shortcuts - note "y" is the year, not the longitude
	result["y"] = prop::year;
	result["m"] = prop::modified;
	result["c"] = prop::created_exif;

	return result;
}

std::vector<prop::prop_scope> prop::search_scopes()
{
	const std::unordered_set<const key*> exclusions
	{
		comment,
		description,
		created_exif,
		created_digitized,
		created_utc,
		disk_num,
		dimensions,
		episode,
		exposure_time,
		focal_length_35mm_equivalent,
		pixel_format,
		iso_speed,
		altitude,
		gps_speed,
		latitude,
		longitude,
		media_category,
		megapixels,
		modified,
		null,
		orientation,
		rating,
		file_size,
		streams,
		track_num,
		unique_id,
		file_name,
		raw_file_name,
		system,
		game,
		crc32c,
		doc_id,
		// Internal storage, not vocabulary: neither has an arm in compare_val, so offering them in
		// the picker gives the user a term that silently matches nothing. `@panorama` is the
		// spelling a user sees for the second one.
		dates_packed,
		panorama,
		// Registered as a name and never stored against an item, so any value the user types for it
		// is compared against nothing.
		encoding_tool,
	};

	std::vector<prop_scope> result;

	for (const auto& i : all_props)
	{
		if (!exclusions.contains(i.second))
		{
			prop_scope s;
			s.scope = i.first;
			s.type = i.second;
			result.emplace_back(s);
		}
	}

	std::ranges::sort(result, [](const prop_scope& left, const prop_scope& right)
	{
		return str::icmp(left.scope, right.scope) < 0;
	});

	result.emplace(result.begin(), "any"s, null);

	return result;
}


std::vector<prop::prop_scope> prop::key_scopes()
{
	// `with:` completes from this list, so a name here is a promise that asking for it answers.
	// `date.pack` is the storage the three dates live in rather than a date anyone asks for, and
	// `digitized` is the old spelling of the concept now called `created` - offering both teaches a
	// fourth date that no longer exists. `panorama` has no arm in has_type for the same reason it has
	// none in compare_val; `@panorama` is the spelling that answers. The rest below are names with no
	// stored value behind them, so `with:` would answer no and `without:` would answer the whole
	// collection - and `null` is not a scope at all.
	const std::unordered_set<const key*> exclusions
	{
		created_digitized,
		dates_packed,
		panorama,
		null,
		crc32c,
		encoding_tool,
		media_category,
		streams,
		unique_id,
	};

	std::vector<prop_scope> result;

	for (const auto& i : all_props)
	{
		if (exclusions.contains(i.second)) continue;

		prop_scope s;
		s.scope = i.first;
		s.type = i.second;
		result.emplace_back(s);
	}

	std::ranges::sort(result, [](const prop_scope& left, const prop_scope& right)
	{
		return str::icmp(left.scope, right.scope) < 0;
	});

	return result;
}

static const property_map properties_by_name = build_properties_by_name();
static const df::hash_map<uint16_t, prop::key_ref> properties_by_id = build_properties_by_id();

search_presence_mask prop::item_metadata::calc_search_presence() const
{
	search_presence_mask result;

	if (!is_null(album)) result.types |= prop::album.search_presence_bit;
	if (!is_null(album_artist)) result.types |= prop::album_artist.search_presence_bit;
	if (!is_null(artist)) result.types |= prop::artist.search_presence_bit;
	if (!is_null(audio_codec)) result.types |= prop::audio_codec.search_presence_bit;
	if (!is_null(audio_sample_type)) result.types |= prop::audio_sample_type.search_presence_bit;
	if (!is_null(audio_sample_rate)) result.types |= prop::audio_sample_rate.search_presence_bit;
	if (!is_null(bitrate)) result.types |= prop::bitrate.search_presence_bit;
	if (!is_null(camera_manufacturer)) result.types |= prop::camera_manufacturer.search_presence_bit;
	if (!is_null(camera_model)) result.types |= prop::camera_model.search_presence_bit;
	if (!is_null(location_place)) result.types |= prop::location_place.search_presence_bit;
	if (!is_null(comment)) result.types |= prop::comment.search_presence_bit;
	if (!is_null(copyright_creator)) result.types |= prop::copyright_creator.search_presence_bit;
	if (!is_null(copyright_credit)) result.types |= prop::copyright_credit.search_presence_bit;
	if (!is_null(copyright_notice)) result.types |= prop::copyright_notice.search_presence_bit;
	if (!is_null(copyright_source)) result.types |= prop::copyright_source.search_presence_bit;
	if (!is_null(copyright_url)) result.types |= prop::copyright_url.search_presence_bit;
	if (!is_null(location_country)) result.types |= prop::location_country.search_presence_bit;
	if (!is_null(description)) result.types |= prop::description.search_presence_bit;
	if (!is_null(file_name)) result.types |= prop::file_name.search_presence_bit;
	if (!is_null(genre)) result.types |= prop::genre.search_presence_bit;
	if (!is_null(lens)) result.types |= prop::lens.search_presence_bit;
	if (!is_null(pixel_format)) result.types |= prop::pixel_format.search_presence_bit;
	if (!is_null(show)) result.types |= prop::show.search_presence_bit;
	if (!is_null(game)) result.types |= prop::game.search_presence_bit;
	if (!is_null(system)) result.types |= prop::system.search_presence_bit;
	if (!is_null(location_state)) result.types |= prop::location_state.search_presence_bit;
	if (!is_null(synopsis)) result.types |= prop::synopsis.search_presence_bit;
	if (!is_null(composer)) result.types |= prop::composer.search_presence_bit;
	if (!is_null(encoder)) result.types |= prop::encoder.search_presence_bit;
	if (!is_null(publisher)) result.types |= prop::publisher.search_presence_bit;
	if (!is_null(performer)) result.types |= prop::performer.search_presence_bit;
	if (!is_null(title)) result.types |= prop::title.search_presence_bit;
	if (!is_null(video_codec)) result.types |= prop::video_codec.search_presence_bit;
	if (!is_null(width)) result.types |= prop::dimensions.search_presence_bit;
	if (!is_null(height)) result.types |= prop::dimensions.search_presence_bit;
	if (!is_null(iso_speed)) result.types |= prop::iso_speed.search_presence_bit;
	if (!is_null(focal_length)) result.types |= prop::focal_length.search_presence_bit;
	if (!is_null(focal_length_35mm_equivalent)) result.types |= prop::focal_length_35mm_equivalent.search_presence_bit;
	if (!is_null(rating)) result.types |= prop::rating.search_presence_bit;
	if (!is_null(season)) result.types |= prop::season.search_presence_bit;
	if (!is_null(track)) result.types |= track_num.search_presence_bit;
	if (!is_null(disk)) result.types |= disk_num.search_presence_bit;
	if (!is_null(duration)) result.types |= prop::duration.search_presence_bit;
	if (!is_null(episode)) result.types |= prop::episode.search_presence_bit;
	if (!is_null(exposure_time)) result.types |= prop::exposure_time.search_presence_bit;
	if (!is_null(f_number)) result.types |= prop::f_number.search_presence_bit;
	if (!is_null(dates.original())) result.types |= prop::created_exif.search_presence_bit;
	if (!is_null(dates.created())) result.types |= prop::created_utc.search_presence_bit;
	if (is_panorama()) result.types |= prop::panorama.search_presence_bit;
	if (!is_null(year)) result.types |= prop::year.search_presence_bit;
	if (orientation != ui::orientation::top_left && orientation != ui::orientation::none)
		result.types |= prop::orientation.search_presence_bit;
	if (coordinate.is_valid()) result.types |= latitude.search_presence_bit;
	if (!is_null(altitude)) result.types |= prop::altitude.search_presence_bit;
	if (!is_null(gps_speed)) result.types |= prop::gps_speed.search_presence_bit;
	if (!is_null(tags)) result.types |= tag.search_presence_bit;
	if (!is_null(label)) result.types |= prop::label.search_presence_bit;
	if (!is_null(doc_id)) result.types |= prop::doc_id.search_presence_bit;

	return result;
}

std::string prop::item_metadata::format(const std::string_view name) const
{
	if (icmp(prop::album.name, name) == 0) return album.str();
	if (icmp(prop::album_artist.name, name) == 0) return album_artist.str();
	if (icmp(prop::artist.name, name) == 0) return artist.str();
	if (icmp(prop::audio_codec.name, name) == 0) return audio_codec.str();
	if (icmp(prop::bitrate.name, name) == 0) return bitrate.str();
	if (icmp(prop::camera_manufacturer.name, name) == 0) return camera_manufacturer.str();
	if (icmp(prop::camera_model.name, name) == 0) return camera_model.str();
	if (icmp(prop::comment.name, name) == 0) return comment.str();
	if (icmp(prop::composer.name, name) == 0) return composer.str();
	if (icmp(prop::copyright_creator.name, name) == 0) return copyright_creator.str();
	if (icmp(prop::copyright_credit.name, name) == 0) return copyright_credit.str();
	if (icmp(prop::copyright_notice.name, name) == 0) return copyright_notice.str();
	if (icmp(prop::copyright_source.name, name) == 0) return copyright_source.str();
	if (icmp(prop::copyright_url.name, name) == 0) return copyright_url.str();
	if (icmp(prop::description.name, name) == 0) return description.str();
	if (icmp(prop::encoder.name, name) == 0) return encoder.str();
	if (icmp(prop::file_name.name, name) == 0) return file_name.str();
	if (icmp(prop::genre.name, name) == 0) return genre.str();
	if (icmp(prop::lens.name, name) == 0) return lens.str();
	if (icmp(prop::location_place.name, name) == 0) return location_place.str();
	if (icmp(prop::location_country.name, name) == 0) return location_country.str();
	if (icmp(prop::location_state.name, name) == 0) return location_state.str();
	if (icmp(prop::performer.name, name) == 0) return performer.str();
	if (icmp(prop::pixel_format.name, name) == 0) return pixel_format.str();
	if (icmp(prop::publisher.name, name) == 0) return publisher.str();
	if (icmp(prop::show.name, name) == 0) return show.str();
	if (icmp(prop::synopsis.name, name) == 0) return synopsis.str();
	if (icmp(prop::title.name, name) == 0) return title.str();
	if (icmp(prop::label.name, name) == 0) return label.str();
	if (icmp(prop::video_codec.name, name) == 0) return video_codec.str();
	if (icmp(prop::raw_file_name.name, name) == 0) return raw_file_name.str();


	return {};
}

std::vector<prop::text_field> prop::descriptive_fields(const item_metadata& md)
{
	// Description leads because it is the field the app can edit and the one most items carry.
	const std::tuple<std::string_view, std::string_view, str::cached> candidates[] = {
		{description.name, tt.prop_name_description, md.description},
		{synopsis.name, tt.prop_name_synopsis, md.synopsis},
		{comment.name, tt.prop_name_comment, md.comment},
	};

	std::vector<text_field> result;

	for (const auto& [id, name, text] : candidates)
	{
		if (is_null(text)) continue;

		const auto duplicate = std::ranges::any_of(result, [t = text](const text_field& f) { return f.text == t; });
		result.emplace_back(id, name, text, duplicate);
	}

	return result;
}

prop::key_ref prop::from_id(const uint16_t id)
{
	const auto found = properties_by_id.find(id);
	return found != properties_by_id.cend() ? found->second : null;
}

prop::key_ref prop::from_prefix(const std::string_view scope)
{
	const auto found = properties_by_name.find(scope);

	if (found != properties_by_name.cend())
	{
		return found->second;
	}

	if (str::icmp(scope, "created") == 0 || str::icmp(scope, tt.query_created) == 0)
	{
		return created_utc;
	}

	if (str::icmp(scope, tt.query_original) == 0)
	{
		return created_exif;
	}

	if (str::icmp(scope, "modified") == 0 || str::icmp(scope, tt.query_modified) == 0)
	{
		return modified;
	}

	return null;
}

std::string prop::format_exposure(const double d)
{
	if (d != 0.0)
	{
		if (d < 1.0)
		{
			return std::format("1/{}s", df::round(1.0 / d));
		}
		return std::format("{}s", df::round(d));
	}

	return {};
}

double prop::closest_fstop(const double fs)
{
	static double stops[] = {
		0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 2, 2.2, 2.4, 2.5, 2.6, 2.8, 3.2, 3.3, 3.4, 3.5, 3.7,
		3.8, 4, 4.4, 4.5, 4.8, 5.0, 5.2, 5.6, 6.2, 6.3, 6.7, 7.1, 7.3, 8, 8.7, 9, 9.5, 10, 11, 12, 13, 14, 15, 16, 17,
		18, 19, 20, 21, 22, 27
	};

	constexpr auto count = std::size(stops);
	auto* const end = stops + count;

	const auto* const upper = std::lower_bound(stops, end, fs);

	if (upper == end) return stops[count - 1];
	if (upper == stops) return *stops;

	const auto* const lower = upper - 1;

	return fabs(*lower - fs) < fabs(*upper - fs) ? *lower : *upper;
}

std::string prop::format_focal_length(const double d, const int filmEquivalent)
{
	if (d != 0.0)
	{
		if (filmEquivalent != 0)
		{
			return std::format("{:.1f}mm ({}mm film eq)", d, filmEquivalent);
		}
		if (d < 1)
			return std::format("{:.1f}mm", d);
		return std::format("{:.0f}mm", d);
	}

	return "0mm"s;
}

std::string prop::format_rating(const int i)
{
	if (i == -1) return std::string(tt.command_rate_rejected);
	if (i >= 1 && i <= 5) return str::to_string(i);
	return std::string(tt.none);
}

std::string prop::format_label(const std::string_view label)
{
	if (str::icmp(label, label_select_text) == 0) return std::string(tt.command_label_select);
	if (str::icmp(label, label_second_text) == 0) return std::string(tt.command_label_second);
	if (str::icmp(label, label_approved_text) == 0) return std::string(tt.command_label_approved);
	if (str::icmp(label, label_review_text) == 0) return std::string(tt.command_label_review);
	if (str::icmp(label, label_to_do_text) == 0) return std::string(tt.command_label_to_do);
	return label.empty() ? std::string(tt.none) : std::string(label);
}

std::string prop::format_iso(const int i)
{
	return std::format("ISO{}", i);
}

std::string prop::format_gps(const double d)
{
	if (fabs(d - gps_coordinate::invalid_coordinate) > 0.001)
	{
		// fixed, not general: "{:.5}" is five significant digits, which loses ~100 m of precision
		return std::format("{:.5f}", d);
	}

	return {};
}

std::string prop::format_gps(const double lat, const double lon)
{
	if (!df::equiv(lat, gps_coordinate::invalid_coordinate) &&
		!df::equiv(lon, gps_coordinate::invalid_coordinate))
	{
		return std::format("{:.5f},{:.5f}", lat, lon);
	}

	return {};
}

std::string prop::format_streams(const int v)
{
	return std::format("{}", v);
}

static constexpr int64_t KB = 1024;
static constexpr int64_t MB = KB * 1024;
static constexpr int64_t GB = MB * 1024;
static constexpr int64_t TB = GB * 1024;


std::string prop::format_bit_rate(const int64_t br)
{
	auto units = "kbit/s";
	auto div = KB;
	auto i = br;

	if (i >= GB)
	{
		div = GB;
		units = "Gbit/s";
	}
	else if (i >= MB)
	{
		div = MB;
		units = "Mbit/s";
	}

	if (i < KB)
	{
		i = KB;
	}

	const auto n = static_cast<int>(i / div);
	const auto r = static_cast<int>(i * 10 / div % 10);

	return std::format("{}{}{} {}", n, platform::number_dec_sep(), r, units);
}


struct magnitude
{
	int64_t n;
	int64_t d;
	char c;
	std::string label;

	bool operator<(const int64_t other) const
	{
		return n < other;
	}
};

std::vector<magnitude> magnitudes{
	{0ll, 0ll, 0, ""},
	{1ll, 1ll, 'B', "<1K"},
	{10ll, 1ll, 'B', "<1K"},
	{100ll, 1ll, 'B', "<1K"},
	{1000ll, 1000ll, 'K', "1K"},
	{10000ll, 1000ll, 'K', "10K"},
	{100000ll, 1000ll, 'K', "100K"},
	{1000000ll, 1000000ll, 'M', "1M"},
	{10000000ll, 1000000ll, 'M', "10M"},
	{100000000ll, 1000000ll, 'M', "100M"},
	{1000000000ll, 1000000000ll, 'G', "1G"},
	{10000000000ll, 1000000000ll, 'G', "10G"},
	{100000000000ll, 1000000000ll, 'G', "100G"},
	{1000000000000ll, 1000000000000ll, 'T', "1T"},
	{10000000000000ll, 1000000000000ll, 'T', "10T"},
	{100000000000000ll, 1000000000000ll, 'T', "100T"},
	{1000000000000000ll, 1000000000000000ll, 'P', "1P"},
	{10000000000000000ll, 1000000000000000ll, 'P', "10P"},
	{100000000000000000ll, 1000000000000000ll, 'P', "100P"},
	{1000000000000000000ll, 1000000000000000000ll, 'E', "1E"},
};

static uint64_t u64_diff(const uint64_t a, const uint64_t b)
{
	return a > b ? a - b : b - a;
}

magnitude find_magnitude(const int64_t n)
{
	const auto ihi = std::lower_bound(magnitudes.begin(), magnitudes.end(), n);

	if (ihi == magnitudes.begin())
	{
		return magnitudes.front();
	}
	if (ihi != magnitudes.end())
	{
		const auto dif_lo = u64_diff((ihi - 1)->n, n);
		const auto dif_hi = u64_diff(ihi->n, n);

		if (dif_lo < dif_hi)
		{
			return *(ihi - 1);
		}
		return *ihi;
	}
	return magnitudes.back();
}


int64_t prop::size_bucket(const int64_t n)
{
	/*static std::array<int64_t, 12> buckets = {
		KB, KB * 10, KB * 100,
		MB, MB * 10, MB * 100,
		GB, GB * 10, GB * 100,
		TB, TB * 10, TB * 100,
	};*/

	return find_magnitude(n).n;
}

std::string prop::format_magnitude(const df::file_size& s)
{
	return find_magnitude(s.to_int64()).label;
}


std::string prop::format_audio_sample_rate(const int v)
{
	return v <= 0 ? std::string{} : format_audio_sample_rate(static_cast<uint32_t>(v));
}

std::string prop::format_audio_sample_rate(const uint32_t v)
{
	const auto remainder = v % 1000 / 100;
	const auto khz = v / 1000;

	if (v > 1000 && remainder == 0) return std::format("{}kHz", khz);
	if (v > 1000) return std::format("{}{}{}kHz", khz, platform::number_dec_sep(), remainder);
	if (v > 0) return std::format("{}Hz", v);
	return {};
}

std::string prop::format_audio_sample_rate(const uint16_t v)
{
	return format_audio_sample_rate(static_cast<uint32_t>(v));
}

std::string prop::format_audio_sample_type(const audio_sample_t v)
{
	switch (v)
	{
	case audio_sample_t::none: return "none"s;
	case audio_sample_t::unsigned_8bit: return "8bit"s;
	case audio_sample_t::signed_16bit: return "16bit"s;
	case audio_sample_t::signed_32bit: return "32bit"s;
	case audio_sample_t::signed_64bit: return "64bit"s;
	case audio_sample_t::signed_float: return "float"s;
	case audio_sample_t::signed_double: return "double"s;
	case audio_sample_t::unsigned_planar_8bit: return "8bit"s;
	case audio_sample_t::signed_planar_16bit: return "16bit"s;
	case audio_sample_t::signed_planar_32bit: return "32bit"s;
	case audio_sample_t::signed_planar_64bit: return "64bit"s;
	case audio_sample_t::planar_float: return "float"s;
	case audio_sample_t::planar_double: return "double"s;
	}
	return {};
}

std::string prop::format_audio_channels(const int v)
{
	switch (v)
	{
	case 1:
		return "mono"s;
	case 2:
		return "stereo"s;
	case 3:
		return "3.0 surround"s;
	case 4:
		return "quad"s;
	case 5:
		return "5.0 surround"s;
	case 6:
		return "5.1 surround"s;
	case 8:
		return "7.1 surround"s;
	default:
		return std::format("{} channels", v);
	}
}

std::string prop::format_f_num(const double d)
{
	// fixed, not general: "{:.01}" is one significant digit, which renders f/2.8 as f/3
	return std::format("f/{:.1f}", d);
}

std::string prop::format_dimensions(const sizei v)
{
	return std::format("{}x{}", v.cx, v.cy);
}

std::string prop::format_video_resolution(const sizei vv)
{
	const auto v = vv.cy > vv.cx ? sizei{vv.cy, vv.cx} : vv;

	if (v.cx == 7680 && v.cy == 4320)
	{
		return "8K"s;
	}
	if (v.cx == 4096)
	{
		return "4K"s;
	}
	if (v.cx == 3840 && v.cy == 2160)
	{
		return "2160p-UHD"s;
	}
	if (v.cx == 2048)
	{
		return "2K"s;
	}
	if (v.cx == 1920 && v.cy == 1200)
	{
		return "WUXGA"s;
	}
	if (v.cx == 2560 && v.cy == 1440)
	{
		return "1440p"s;
	}
	if (v.cx == 1920 && v.cy == 1080)
	{
		return "1080p"s;
	}
	if (v.cx == 1280 && v.cy == 720)
	{
		return "720p"s;
	}
	if (v.cx == 1280 && v.cy == 720)
	{
		return "720p"s;
	}
	if (v.cx == 854 && v.cy == 480)
	{
		return "480p"s;
	}
	if (v.cx == 640 && v.cy == 360)
	{
		return "360p"s;
	}
	if (v.cx == 426 && v.cy == 240)
	{
		return "240p"s;
	}
	if (v.cx == 640 && v.cy == 360)
	{
		return "360p"s;
	}
	if (v.cx == 480 && v.cy == 360)
	{
		return "360p"s;
	}
	if (v.cx == 320 && v.cy == 240)
	{
		return "240p"s;
	}
	if (v.cx == 320 && v.cy == 240)
	{
		return "240p"s;
	}
	if (v.cx == 320 && v.cy == 180)
	{
		return "180p"s;
	}
	if (v.cx == 256 && v.cy == 144)
	{
		return "144p"s;
	}
	if (v.cx == 256 && v.cy == 144)
	{
		return "144p"s;
	}
	if (v.cx == 176 && v.cy == 144)
	{
		return "144p"s;
	}
	if (v.cx == 160 && v.cy == 120)
	{
		return "120p"s;
	}

	return {};
}

std::string_view text_or_default(const std::string_view text, const std::string_view def)
{
	return str::is_empty(text) ? def : text;
}

std::string prop::replace_tokens(const std::string_view name_template, const item_metadata_const_ptr& md,
                                 std::string_view name, df::date_t created)
{
	auto substitute = [md, name, created](std::ostringstream& result, const std::string_view token_in)
	{
		const auto token = str::to_lower(token_in);

		if (token == "name" || token == "file" || token == "filename")
		{
			result << name;
		}
		else if (token == "created" || token == "created.date")
		{
			if (created.is_valid())
			{
				result << std::format("{:04}-{:02}-{:02}", created.year(), created.month(), created.day());
			}
			else
			{
				result << tt.unknown.sv();
			}
		}
		else if (token == "year" || token == "created.year")
		{
			if (md && md->year)
			{
				result << md->year;
			}
			else if (created.is_valid())
			{
				const std::ios_base::fmtflags f(result.flags());
				result << std::setfill('0') << std::setw(4) << created.year();
				result.flags(f);
			}
			else
			{
				result << tt.unknown.sv();
			}
		}
		else if (token == "month" || token == "created.month")
		{
			if (created.is_valid())
			{
				const std::ios_base::fmtflags f(result.flags());
				result << std::setfill('0') << std::setw(2) << created.month();
				result.flags(f);
			}
			else
			{
				result << tt.unknown.sv();
			}
		}
		else if (token == "month.text")
		{
			if (created.is_valid())
			{
				result << str::month(created.month(), true);
			}
			else
			{
				result << tt.unknown.sv();
			}
		}
		else if (token == "month.short")
		{
			if (created.is_valid())
			{
				result << str::short_month(created.month(), true);
			}
			else
			{
				result << tt.unknown.sv();
			}
		}
		else if (token == "day" || token == "created.day")
		{
			if (created.is_valid())
			{
				const std::ios_base::fmtflags f(result.flags());
				result << std::setfill('0') << std::setw(2) << created.day();
				result.flags(f);
			}
			else
			{
				result << tt.unknown.sv();
			}
		}
		else if (md)
		{
			if (token == "artist")
				result << text_or_default(
					md->album_artist.is_empty() ? md->artist.sv() : md->album_artist.sv(), tt.unknown);
			else if (token == "album") result << text_or_default(md->album, tt.unknown);
			else if (token == "show") result << text_or_default(md->show, tt.unknown);
			else if (token == "season") result << text_or_default(str::to_string(md->season), tt.unknown);
			else if (token == "country") result << text_or_default(md->location_country, tt.unknown);
			else
			{
				result << text_or_default(md->format(token), tt.unknown);
			}
		}
		else
		{
			result << tt.unknown.sv();
		}
	};

	return str::replace_tokens(name_template, substitute);
}

std::string prop::format_pixels(const sizei v, const file_type_ref ft)
{
	if (v.is_empty())
	{
		return std::string(tt.resolution_none);
	}

	if (ft->has_trait(file_traits::av))
	{
		const auto video_res = format_video_resolution(v);
		if (!video_res.empty()) return video_res;
	}

	if (ft->has_trait(file_traits::bitmap))
	{
		if (v.cx <= 128 && v.cy <= 128)
		{
			return std::string(tt.pixels_icon);
		}

		const auto mp = ui::calc_mega_pixels(v.cx, v.cy);

		if (mp < 0.5)
		{
			return std::string(tt.pixels_small);
		}

		if (mp >= 2.0)
		{
			return str::print("%dmp", df::round(mp));
		}

		return str::print("%1.1fmp", mp);
	}

	return std::format("{}x{}", v.cx, v.cy);
}

std::string prop::format_duration(const int n)
{
	return str::format_seconds(n);
}

std::string prop::format_date(const df::date_t d)
{
	return d.is_valid() ? platform::format_date(d) : std::string{};
}

std::string df::xy32::str() const
{
	if (y == 0)
	{
		return str::to_string(x);
	}

	return std::format("{}/{}", x, y);
}

std::string df::xy8::str() const
{
	if (y == 0)
	{
		return str::to_string(x);
	}

	return std::format("{}/{}", x, y);
}
