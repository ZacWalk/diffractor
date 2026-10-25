// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: XMP (Extensible Metadata Platform) support. Maps Diffractor's properties to and from XMP
// packets, whose data model is libfiles', and reads and writes the packets in files with libfiles.

#include "pch.h"

#include "model_property.h"
#include "model_location.h"
#include "metadata_xmp.h"
#include "files.h"

#include "libfiles/write.h"
#include "libfiles/xmp.h"

namespace xmp_ns = libfiles::xmp::ns;

// ReSharper disable CppUnusedIncludeDirective
#include "metadata_exif.h"

static df::file_path s_test_raw_source_xmp_path;
static df::blob s_test_raw_source_xmp;
static std::vector<int> s_test_update_failure_sequence;

void metadata_xmp::set_test_raw_source_xmp(const df::file_path path, const df::cspan xmp)
{
	s_test_raw_source_xmp_path = path;
	s_test_raw_source_xmp.assign(xmp.begin(), xmp.end());
}

void metadata_xmp::set_test_update_failure_sequence(std::vector<int> actions)
{
	s_test_update_failure_sequence = std::move(actions);
}

// A malformed sidecar or a bad batch of files repeats the same failure for every scanned item, so
// each distinct message is written once and the total is capped; the session total lands in the
// exit perf summary.
static void record_xmp_error(const std::string_view context, const std::string_view message)
{
	df::bump(df::file_perf.metadata_errors);
	df::log_once(context, message);
}

static df::date_t xmp_parse_date(const std::string_view str)
{
	if (!str.empty())
	{
		df::date_t ft;
		ft.parse_xml_date(str);

		if (ft.is_valid())
		{
			return ft;
		}
	}

	return {};
}

// parse_xml_date reads the digits and drops the zone, so the caller has to recover it. Without
// this an XMP date ending in Z is filed as a local reading and lands an hour or more away from the
// container date it is meant to agree with, which then reads as two dates instead of one.
static int16_t xmp_date_offset(const std::string_view str)
{
	return prop::parse_utc_offset(str);
}

static bool xmp_parse_coordinate_part(const std::string_view text, double& result)
{
	if (text.empty()) return false;
	const auto* const begin = text.data();
	const auto* const end = begin + text.size();
	const auto parsed = std::from_chars(begin, end, result);
	return parsed.ec == std::errc{} && parsed.ptr == end && result >= 0.0;
}

static bool xmp_decode_gps_coordinate(const std::string_view str, const bool is_latitude, double& result)
{
	const auto text = str::trim(str);
	const auto len = text.size();
	if (len < 4) return false;

	// DDD,MM,SSk
	// DDD,MM.mmk

	// Key:
	// DDD = number of degrees
	// MM = number of minutes
	// SS = number of seconds
	// mm = fraction of minutes
	// k = {N/S/E/W}

	const auto hemisphere = text[len - 1];
	const auto north = hemisphere == 'N' || hemisphere == 'n';
	const auto south = hemisphere == 'S' || hemisphere == 's';
	const auto east = hemisphere == 'E' || hemisphere == 'e';
	const auto west = hemisphere == 'W' || hemisphere == 'w';

	if (is_latitude ? !(north || south) : !(east || west)) return false;

	const auto body = text.substr(0, len - 1);
	const auto first_comma = body.find(',');
	if (first_comma == std::string_view::npos) return false;

	const auto second_comma = body.find(',', first_comma + 1);
	if (second_comma != std::string_view::npos && body.find(',', second_comma + 1) != std::string_view::npos)
		return false;

	double degrees = 0.0;
	double minutes = 0.0;
	double seconds = 0.0;

	if (!xmp_parse_coordinate_part(body.substr(0, first_comma), degrees)) return false;

	if (second_comma == std::string_view::npos)
	{
		if (!xmp_parse_coordinate_part(body.substr(first_comma + 1), minutes)) return false;
	}
	else
	{
		if (!xmp_parse_coordinate_part(body.substr(first_comma + 1, second_comma - first_comma - 1), minutes))
			return false;
		if (!xmp_parse_coordinate_part(body.substr(second_comma + 1), seconds)) return false;
	}

	if (minutes >= 60.0 || seconds >= 60.0) return false;

	const auto coordinate = gps_coordinate::dms_to_decimal(degrees, minutes, seconds);
	const auto signed_coordinate = (south || west) ? -coordinate : coordinate;
	const auto candidate = is_latitude ? gps_coordinate(signed_coordinate, 0.0) : gps_coordinate(0.0, signed_coordinate);

	if (!candidate.is_valid()) return false;
	result = signed_coordinate;
	return true;
}

static bool xmp_decode_rational(const std::string_view text, metadata_exif::urational32_t& result)
{
	const auto len = text.size();
	unsigned long locNum = 0, locDenom = 0;
	char nextChar = 0; // Used to make sure sscanf consumes all of the string.
	const auto* const sz = std::bit_cast<const char*>(text.data());

	const int items = _snscanf_s(sz, len, "%lu/%lu%c", &locNum, &locDenom, &nextChar, 1);
	// AUDIT: This is safe, check the calls.

	if (items != 2)
	{
		if (items != 1) return false;
		locDenom = 1; // The XMP was just an integer, assume a denominator of 1.
	}

	// Protect against division by zero
	if (locDenom == 0)
	{
		return false;
	}

	result.numerator = locNum;
	result.denominator = locDenom;
	return true;
}

std::string microsoft_photo_prefix;

// xmp:Rating is -1 (rejected) through 5. Anything else came from a broken writer.
static int xmp_safe_rating(const int r)
{
	return std::clamp(r, -1, 5);
}

static str::cached xmp_load_array(const libfiles::xmp::meta& xmp, const std::string_view schema_ns,
                                  const std::string_view array_name)
{
	str::cached result = {};
	const auto count = xmp.count_array_items(schema_ns, array_name);

	if (count > 0)
	{
		std::vector<std::string> parts;
		parts.reserve(count);

		for (auto i = 0; i < count; i++)
		{
			std::string str;

			if (xmp.get_array_item(schema_ns, array_name, i + 1, &str))
			{
				parts.emplace_back(str::utf8_cast(str));
			}
		}

		if (!parts.empty())
		{
			result = str::cache(str::combine(parts));
		}
	}

	return result;
}

// Google's photo sphere namespace, which is what a phone panorama mode and most stitchers write.
// It is not one the toolkit knows, so it is registered at startup before any file is parsed.
constexpr auto ns_gpano = "http://ns.google.com/photos/1.0/panorama/";

// Records what the file itself declares, never what its shape suggests. `UsePanoramaViewer` and the
// cropped-area properties are read only to answer "is this a panorama" for a writer that omitted
// ProjectionType; a file carrying none of them is not one.
static void read_panorama_projection(const libfiles::xmp::meta& xmp, prop::item_metadata& md)
{
	std::string utf8;

	if (xmp.get_property(ns_gpano, "ProjectionType", &utf8))
	{
		const auto text = str::trim(str::utf8_cast(utf8));

		if (str::icmp(text, "equirectangular") == 0)
		{
			md.panorama = prop::panorama_projection::equirectangular;
			return;
		}

		if (str::icmp(text, "cylindrical") == 0)
		{
			md.panorama = prop::panorama_projection::cylindrical;
			return;
		}

		if (!text.empty())
		{
			md.panorama = prop::panorama_projection::unspecified;
			return;
		}
	}

	for (const auto* const declares_panorama : {
		     "UsePanoramaViewer", "FullPanoWidthPixels", "CroppedAreaImageWidthPixels"
	     })
	{
		if (xmp.get_property(ns_gpano, declares_panorama, &utf8))
		{
			md.panorama = prop::panorama_projection::unspecified;
			return;
		}
	}
}

static void parse_xmp(const libfiles::xmp::meta& xmp, prop::item_metadata& md)
{
	std::string utf8;

	if (xmp.get_property(xmp_ns::photoshop, "DateCreated", &utf8))
	{
		const auto text = str::utf8_cast(utf8);
		const auto d = xmp_parse_date(text);
		if (d.is_valid()) md.dates.add(prop::date_source::photoshop_created, d, xmp_date_offset(text));
	}

	if (xmp.get_property(xmp_ns::xmp, "CreateDate", &utf8))
	{
		const auto text = str::utf8_cast(utf8);
		const auto d = xmp_parse_date(text);

		if (d.is_valid())
		{
			md.dates.add(prop::date_source::xmp_create, d, xmp_date_offset(text));
		}
		else if (str::is_num(text))
		{
			md.year = str::to_int(text);
		}
	}

	if (xmp.get_property(xmp_ns::xmp, "ModifyDate", &utf8))
	{
		const auto text = str::utf8_cast(utf8);
		const auto d = xmp_parse_date(text);
		if (d.is_valid()) md.dates.add(prop::date_source::xmp_modify, d, xmp_date_offset(text));
	}

	if (xmp.get_property(xmp_ns::exif, "DateTimeDigitized", &utf8))
	{
		const auto text = str::utf8_cast(utf8);
		const auto d = xmp_parse_date(text);
		if (d.is_valid()) md.dates.add(prop::date_source::xmp_exif_digitized, d, xmp_date_offset(text));
	}

	if (xmp.get_property(xmp_ns::exif, "DateTimeOriginal", &utf8))
	{
		const auto text = str::utf8_cast(utf8);
		const auto d = xmp_parse_date(text);
		if (d.is_valid()) md.dates.add(prop::date_source::xmp_exif_original, d, xmp_date_offset(text));
	}

	// A coordinate that failed to decode must not be applied - a zeroed half pins the item to the
	// Gulf of Guinea instead of leaving the location unknown. Applied as a pair for the same reason:
	// a file carrying only one XMP component would otherwise keep the other from EXIF, pinning the
	// item to a coordinate neither source states.
	{
		double latitude = 0;
		double longitude = 0;

		const auto has_latitude = xmp.get_property(xmp_ns::exif, "GPSLatitude", &utf8) &&
			xmp_decode_gps_coordinate(str::utf8_cast(utf8), true, latitude);
		const auto has_longitude = xmp.get_property(xmp_ns::exif, "GPSLongitude", &utf8) &&
			xmp_decode_gps_coordinate(str::utf8_cast(utf8), false, longitude);

		if (has_latitude && has_longitude)
		{
			const gps_coordinate candidate(latitude, longitude);
			if (candidate.is_valid()) md.coordinate = candidate;
		}
	}

	if (xmp.get_property(xmp_ns::photoshop, "City", &utf8))
	{
		md.location_place = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::photoshop, "State", &utf8))
	{
		md.location_state = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::photoshop, "Country", &utf8))
	{
		md.location_country = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::photoshop, "Credit", &utf8))
	{
		md.copyright_credit = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::xmp_rights, "WebStatement", &utf8))
	{
		md.copyright_url = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::photoshop, "Source", &utf8))
	{
		md.copyright_source = str::strip_and_cache(utf8);
	}

	// Xmp is parsed last, so an absent array must not erase what Exif or Iptc supplied.
	if (const auto creator = xmp_load_array(xmp, xmp_ns::dc, "creator"); !str::is_empty(creator))
	{
		md.copyright_creator = creator;
	}

	if (xmp.get_localized_text(xmp_ns::dc, "rights", "", "x-default", nullptr, &utf8))
	{
		md.copyright_notice = str::strip_and_cache(utf8);
	}

	if (xmp.get_localized_text(xmp_ns::dc, "title", "", "x-default", nullptr, &utf8))
	{
		md.title = str::strip_and_cache(utf8);
	}

	if (xmp.get_localized_text(xmp_ns::exif, "UserComment", "", "x-default", nullptr, &utf8))
	{
		md.comment = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::dm, "logComment", &utf8))
	{
		md.comment = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::dm, "synopsis", &utf8))
	{
		md.synopsis = str::strip_and_cache(utf8);
	}

	if (xmp.get_localized_text(xmp_ns::dc, "description", "", "x-default", nullptr, &utf8))
	{
		md.description = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::xmp, "Rating", &utf8))
	{
		md.rating = xmp_safe_rating(str::to_int(str::utf8_cast(utf8)));
	}
	else if (xmp.get_property(xmp_ns::microsoft_photo, "Rating", &utf8))
	{
		//MicrosoftPhoto:Rating
		md.rating = xmp_safe_rating(df::round_up(str::to_int(str::utf8_cast(utf8)), 20));
	}

	if (xmp.get_property(xmp_ns::xmp, "Label", &utf8))
	{
		md.label = str::strip_and_cache(utf8);
	}

	// Xmp is parsed last, so an array that is simply absent must not erase what Exif or Iptc
	// supplied. An array that is present and empty is different: that is the file stating it has no
	// tags, and the writer above emits exactly that for cleared tags rather than deleting the
	// property. Reading only the non-empty case threw that statement away, so clearing every tag
	// and rescanning brought the legacy Iptc keywords straight back.
	if (xmp.exists(xmp_ns::dc, "subject"))
	{
		md.tags = xmp_load_array(xmp, xmp_ns::dc, "subject");
	}

	if (xmp.get_property(xmp_ns::tiff, "Make", &utf8))
	{
		md.camera_manufacturer = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::tiff, "Model", &utf8))
	{
		md.camera_model = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::dm, "album", &utf8))
	{
		md.album = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::dm, "albumArtist", &utf8))
	{
		md.album_artist = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::dm, "artist", &utf8))
	{
		md.artist = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::dm, "genre", &utf8))
	{
		md.genre = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::dm, "show", &utf8))
	{
		md.show = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::dm, "trackNumber", &utf8))
	{
		md.track = df::xy8::parse(str::utf8_cast(utf8));
	}

	if (xmp.get_property(xmp_ns::dm, "discNumber", &utf8))
	{
		md.disk = df::xy8::parse(str::utf8_cast(utf8));
	}

	if (xmp.get_property(xmp_ns::dm, "episode", &utf8))
	{
		md.episode = df::xy8::parse(str::utf8_cast(utf8));
	}

	if (xmp.get_property(xmp_ns::dm, "season", &utf8))
	{
		md.season = str::to_int(str::utf8_cast(utf8));
	}

	// <exif:FNumber>33/10</exif:FNumber>
	// <exif:FocalLength>600/10</exif:FocalLength>
	// <exif:ExposureTime>10/100</exif:ExposureTime>

	if (xmp.get_property(xmp_ns::exif, "FocalLengthIn35mmFilm", &utf8))
	{
		metadata_exif::urational32_t r;

		if (xmp_decode_rational(str::utf8_cast(utf8), r))
		{
			md.focal_length_35mm_equivalent = r.round();
		}
	}

	if (xmp.get_property(xmp_ns::exif, "FNumber", &utf8))
	{
		metadata_exif::urational32_t r;

		if (xmp_decode_rational(str::utf8_cast(utf8), r))
		{
			md.f_number = static_cast<float>(r.to_real());
		}
	}

	if (xmp.get_property(xmp_ns::exif, "FocalLength", &utf8))
	{
		metadata_exif::urational32_t r;

		if (xmp_decode_rational(str::utf8_cast(utf8), r))
		{
			md.focal_length = static_cast<float>(r.to_real());
		}
	}

	if (xmp.get_property(xmp_ns::exif, "ExposureTime", &utf8))
	{
		metadata_exif::urational32_t r;

		if (xmp_decode_rational(str::utf8_cast(utf8), r))
		{
			md.exposure_time = static_cast<float>(r.to_real());
		}
	}

	if (xmp.get_property(xmp_ns::tiff, "Orientation", &utf8))
	{
		// An out of range value would index the rotation tables with an undefined enumerator.
		const auto o = str::to_int(str::utf8_cast(utf8));
		if (o >= 1 && o <= 8) md.orientation = static_cast<ui::orientation>(o);
	}

	if (xmp.get_property(xmp_ns::exif_aux, "Lens", &utf8))
	{
		md.lens = str::strip_and_cache(utf8);
	}

	if (xmp.get_property(xmp_ns::camera_raw, "RawFileName", &utf8))
	{
		md.raw_file_name = str::strip_and_cache(utf8);
	}

	read_panorama_projection(xmp, md);
}


void metadata_xmp::initialise()
{
	libfiles::xmp::register_namespace(ns_gpano, "GPano");
}

void metadata_edits::apply(libfiles::xmp::meta& meta) const
{
	if (location_coordinate.has_value())
	{
		const auto position = location_coordinate.value();

		meta.set_property(xmp_ns::exif, "GPSLatitude",
		                 str::utf8_cast2(gps_coordinate::decimal_to_dms_str(position.latitude(), true)));
		meta.set_property(xmp_ns::exif, "GPSLongitude",
		                 str::utf8_cast2(gps_coordinate::decimal_to_dms_str(position.longitude(), false)));
	}


	if (location_place.has_value())
	{
		if (!str::is_empty(location_place.value()))
		{
			meta.set_property(xmp_ns::photoshop, "City", str::utf8_cast2(location_place.value()));
		}
		else
		{
			meta.delete_property(xmp_ns::photoshop, "City");
		}
	}

	if (location_state.has_value())
	{
		if (!str::is_empty(location_state.value()))
		{
			meta.set_property(xmp_ns::photoshop, "State", str::utf8_cast2(location_state.value()));
		}
		else
		{
			meta.delete_property(xmp_ns::photoshop, "State");
		}
	}

	if (location_country.has_value())
	{
		if (!str::is_empty(location_country.value()))
		{
			meta.set_property(xmp_ns::photoshop, "Country", str::utf8_cast2(location_country.value()));
		}
		else
		{
			meta.delete_property(xmp_ns::photoshop, "Country");
		}
	}

	if (copyright_credit.has_value())
	{
		meta.set_property(xmp_ns::photoshop, "Credit", str::utf8_cast2(copyright_credit.value()));
	}

	if (copyright_url.has_value())
	{
		meta.set_property(xmp_ns::xmp_rights, "WebStatement", str::utf8_cast2(copyright_url.value()));
	}

	if (copyright_source.has_value())
	{
		meta.set_property(xmp_ns::photoshop, "Source", str::utf8_cast2(copyright_source.value()));
	}

	if (copyright_notice.has_value())
	{
		meta.set_localized_text(xmp_ns::dc, "rights", "", "x-default", str::utf8_cast2(copyright_notice.value()));
	}

	if (copyright_creator.has_value())
	{
		// dc:creator is an ordered array of names, and that is how it is read back. Writing it as
		// alt-lang text produces a packet Adobe tools will not show.
		meta.delete_property(xmp_ns::dc, "creator");

		if (str::is_empty(copyright_creator.value()))
		{
			meta.set_composite(xmp_ns::dc, "creator", libfiles::xmp::array_is_ordered);
		}
		else
		{
			// Names are separated by punctuation, never by the space inside "Jane Doe".
			for (const auto& part : str::split(copyright_creator.value(), true, str::is_artist_separator))
			{
				const auto name = str::trim(part);
				if (!name.empty())
				{
					meta.append_array_item(xmp_ns::dc, "creator", libfiles::xmp::array_is_ordered, str::utf8_cast2(name));
				}
			}
		}
	}

	if (title.has_value())
	{
		meta.set_localized_text(xmp_ns::dc, "title", "", "x-default", str::utf8_cast2(title.value()));
	}

	if (description.has_value())
	{
		meta.set_localized_text(xmp_ns::dc, "description", "", "x-default", str::utf8_cast2(description.value()));
	}

	if (comment.has_value())
	{
		meta.set_property(xmp_ns::dm, "logComment", str::utf8_cast2(comment.value()));
	}

	if (synopsis.has_value())
	{
		meta.set_property(xmp_ns::dm, "synopsis", str::utf8_cast2(synopsis.value()));
	}

	if (rating.has_value())
	{
		const int r = std::clamp(rating.value(), -1, 5);
		meta.set_property_int(xmp_ns::xmp, "Rating", r);
		meta.set_property_int(xmp_ns::microsoft_photo, "Rating", rating_to_percent(r));
	}

	if (label.has_value())
	{
		if (!str::is_empty(label.value()))
		{
			meta.set_property(xmp_ns::xmp, "Label", str::utf8_cast2(label.value()));
		}
		else
		{
			meta.delete_property(xmp_ns::xmp, "Label");
		}
	}

	if (album.has_value())
	{
		meta.set_property(xmp_ns::dm, "album", str::utf8_cast2(album.value()));
	}

	if (album_artist.has_value())
	{
		meta.set_property(xmp_ns::dm, "albumArtist", str::utf8_cast2(album_artist.value()));
	}

	if (genre.has_value())
	{
		meta.set_property(xmp_ns::dm, "genre", str::utf8_cast2(genre.value()));
	}

	if (show.has_value())
	{
		meta.set_property(xmp_ns::dm, "show", str::utf8_cast2(show.value()));
	}

	if (year.has_value() && !date_created.has_value())
	{
		// A bare year and a Created date are the same XMP property, told apart on read by whether it
		// parses as a date. Writing both would leave whichever went last, so the date wins: it is the
		// more specific answer and it still carries the year.
		meta.set_property(xmp_ns::xmp, "CreateDate", str::utf8_cast2(str::to_string(year.value())));
	}

	if (date_created.has_value())
	{
		const auto text = str::utf8_cast2(date_created.value().to_xmp_date());

		// Both, as Original writes both below: xmp:CreateDate is the fourth-ranked Created source, so
		// wherever the file's own DateTimeDigitized survives the write it would outrank the edit and
		// the correction would appear to do nothing.
		meta.set_property(xmp_ns::xmp, "CreateDate", text);
		meta.set_property(xmp_ns::exif, "DateTimeDigitized", text);
	}

	if (date_original.has_value())
	{
		const auto text = str::utf8_cast2(date_original.value().to_xmp_date());

		// Both, because photoshop:DateCreated alone is the lowest-authority capture source and would
		// be outranked by the file's own DateTimeOriginal - the edit would appear to do nothing. The
		// toolkit reconciles exif:DateTimeOriginal back into the embedded EXIF on save.
		meta.set_property(xmp_ns::photoshop, "DateCreated", text);
		meta.set_property(xmp_ns::exif, "DateTimeOriginal", text);
	}

	if (episode.has_value())
	{
		meta.set_property(xmp_ns::dm, "episode", str::utf8_cast2(episode.value().str()));
	}

	if (season.has_value())
	{
		meta.set_property(xmp_ns::dm, "season", str::utf8_cast2(str::to_string(season.value())));
	}

	if (track_num.has_value())
	{
		meta.set_property(xmp_ns::dm, "trackNumber", str::utf8_cast2(track_num.value().str()));
	}

	if (disk_num.has_value())
	{
		meta.set_property(xmp_ns::dm, "discNumber", str::utf8_cast2(disk_num.value().str()));
	}

	if (artist.has_value())
	{
		meta.set_property(xmp_ns::dm, "artist", str::utf8_cast2(artist.value()));
	}

	if (orientation.has_value())
	{
		meta.set_property(xmp_ns::tiff, "Orientation",
		                 str::utf8_cast2(str::to_string(static_cast<int>(orientation.value()))), libfiles::xmp::delete_existing);
	}

	if (!remove_tags.is_empty() || !add_tags.is_empty())
	{
		tag_set tags;
		const auto count = meta.count_array_items(xmp_ns::dc, "subject");

		for (auto i = 0; i < count; i++)
		{
			std::string str;
			if (meta.get_array_item(xmp_ns::dc, "subject", i + 1, &str))
			{
				tags.add_one(str::cache(str));
			}
		}

		tags.remove(remove_tags);
		tags.add(add_tags);
		tags.make_unique();

		meta.delete_property(xmp_ns::dc, "subject");

		if (tags.is_empty())
		{
			meta.set_composite(xmp_ns::dc, "subject", libfiles::xmp::value_is_array);
		}
		else
			for (auto i = 0u; i < tags.size(); i++)
			{
				meta.append_array_item(xmp_ns::dc, "subject", libfiles::xmp::value_is_array, str::utf8_cast2(tags[i]));
			}

		// Sync exif:XPKeywords (Windows Explorer tag) with dc:subject. Cleared tags are written
		// as an empty value rather than a deleted property: the TIFF export leaves tags that are
		// simply absent from XMP alone, so deleting here would leave the old keywords in place.
		meta.set_property(xmp_ns::exif, "XPKeywords",
		                 tags.is_empty() ? std::string() : str::utf8_cast2(tags.to_string(";", false)));
	}

	if (remove_rating)
	{
		meta.set_property_int(xmp_ns::xmp, "Rating", 0);
		meta.set_property_int(xmp_ns::microsoft_photo, "Rating", 0);
	}
}

// A packet from a JPEG APP1 segment still starts with its signature, which already ends in the NUL
// that terminates the segment header.
static std::string_view packet_text(const df::cspan xmp)
{
	std::string_view text(std::bit_cast<const char*>(xmp.data), xmp.size);
	if (text.size() > xmp_signature.size() && text.starts_with(xmp_signature)) text.remove_prefix(xmp_signature.size());
	return text;
}

metadata_xmp::property_presence metadata_xmp::properties(const df::cspan xmp)
{
	property_presence result;

	try
	{
		if (xmp.size == 0) return result;

		libfiles::xmp::meta meta;
		meta.parse(packet_text(xmp));

		result.tags = meta.exists(xmp_ns::dc, "subject");
		result.rating = meta.exists(xmp_ns::xmp, "Rating") ||
			meta.exists(xmp_ns::microsoft_photo, "Rating");
	}
	catch (const std::exception& e)
	{
		record_xmp_error(__FUNCTION__, e.what());
	}

	return result;
}

void metadata_xmp::parse(prop::item_metadata& pd, const df::cspan xmp)
{
	try
	{
		if (xmp.size > 0)
		{
			libfiles::xmp::meta meta;
			meta.parse(packet_text(xmp));
			parse_xmp(meta, pd);
		}
	}
	catch (const std::exception& e)
	{
		record_xmp_error(__FUNCTION__, e.what());
	}
}


void metadata_xmp::parse(prop::item_metadata& pd, const df::file_path path)
{
	try
	{
		libfiles::xmp::meta xmp;
		auto f = blob_from_file(path);

		if (!f.empty())
		{
			xmp.parse(std::string_view(std::bit_cast<const char*>(f.data()), f.size()));
			parse_xmp(xmp, pd);
		}
	}
	catch (const std::exception& e)
	{
		record_xmp_error(__FUNCTION__, e.what());
	}
}

df::file_path probe_xmp_path(const df::file_path src_path, const std::string_view xmp_name)
{
	if (!xmp_name.empty())
	{
		return src_path.folder().combine_file(xmp_name);
	}

	return src_path.extension(".xmp");
}

static std::optional<libfiles::packet_reading> read_file_packet(df::file_path path);

// GPano writes these as plain integers. A property the file omits leaves its member zero, which
// prop::panorama_geometry reads as an incomplete declaration and resolves against the pixels.
static void read_panorama_geometry(const libfiles::xmp::meta& xmp, prop::panorama_geometry& g)
{
	const auto read = [&xmp](const char* const name, int& out)
	{
		std::int32_t value = 0;
		if (xmp.get_property_int(ns_gpano, name, &value)) out = value;
	};

	read("FullPanoWidthPixels", g.full_width);
	read("FullPanoHeightPixels", g.full_height);
	read("CroppedAreaLeftPixels", g.cropped_left);
	read("CroppedAreaTopPixels", g.cropped_top);
	read("CroppedAreaImageWidthPixels", g.cropped_width);
	read("CroppedAreaImageHeightPixels", g.cropped_height);
}

prop::panorama_geometry metadata_xmp::panorama(const df::file_path path)
{
	prop::panorama_geometry result;

	try
	{
		const auto* const ft = files::file_type_from_name(path);

		if (ft->has_trait(file_traits::embedded_xmp))
		{
			if (const auto packet = read_file_packet(path); packet && !packet->packet.empty())
			{
				libfiles::xmp::meta xmp;
				xmp.parse(packet->packet);
				read_panorama_geometry(xmp, result);
				if (result.is_valid()) return result;
			}
		}

		// A stitcher that wrote a sidecar rather than an embedded packet still declared the sphere. The
		// embedded reading is discarded first: read_panorama_geometry fills only what a packet declares,
		// so joining an incomplete embedded declaration to the sidecar's would describe neither file.
		result = {};

		const auto sidecar = blob_from_file(probe_xmp_path(path, {}));

		if (!sidecar.empty())
		{
			libfiles::xmp::meta xmp;
			xmp.parse(std::string_view(std::bit_cast<const char*>(sidecar.data()), sidecar.size()));
			read_panorama_geometry(xmp, result);
		}
	}
	catch (const std::exception& e)
	{
		record_xmp_error(__FUNCTION__, e.what());
	}

	return result;
}

// The formats libfiles reads and writes: every one diffractor embeds metadata in.
static libfiles::file_format libfiles_update_format(const df::file_path path)
{
	const auto f = open_file(path, platform::file_open_mode::read);
	uint8_t head[16] = {};

	if (!f || f->read(head, sizeof(head)) != sizeof(head)) return libfiles::file_format::unknown;

	return libfiles::detect_file_format(head);
}

// The packet a file holds, where libfiles reads and writes it. Nothing when libfiles does not read the
// format or the file cannot be opened.
static std::optional<libfiles::packet_reading> read_file_packet(const df::file_path path)
{
	const auto f = open_file(path, platform::file_open_mode::read);
	if (!f) return {};

	platform_file_stream stream(f);
	libfiles::reader r(stream);
	if (r.size() < 16) return {};

	uint8_t head[16];
	r.read(0, head, sizeof(head));
	if (!libfiles::can_update(libfiles::detect_file_format(head))) return {};

	return libfiles::read_packet(r);
}

// What the item shows, written into the packet wherever the packet lacks it.
static void seed_from_shown(const prop::item_metadata& shown, libfiles::xmp::meta& xmp)
{
	metadata_edits seed;

	const auto missing = [&xmp](const std::string_view ns, const std::string_view name)
	{
		return !xmp.get_property(ns, name);
	};

	const auto text = [](const str::cached value) -> std::optional<std::string>
	{
		if (str::is_empty(value)) return {};
		return std::string(value.sv());
	};

	if (missing(xmp_ns::dc, "title")) seed.title = text(shown.title);
	if (missing(xmp_ns::dc, "description")) seed.description = text(shown.description);
	if (missing(xmp_ns::dc, "rights")) seed.copyright_notice = text(shown.copyright_notice);
	if (missing(xmp_ns::dc, "creator")) seed.copyright_creator = text(shown.copyright_creator);
	if (missing(xmp_ns::photoshop, "Credit")) seed.copyright_credit = text(shown.copyright_credit);
	if (missing(xmp_ns::photoshop, "Source")) seed.copyright_source = text(shown.copyright_source);
	if (missing(xmp_ns::photoshop, "City")) seed.location_place = text(shown.location_place);
	if (missing(xmp_ns::photoshop, "State")) seed.location_state = text(shown.location_state);
	if (missing(xmp_ns::photoshop, "Country")) seed.location_country = text(shown.location_country);
	if (missing(xmp_ns::dc, "subject") && !str::is_empty(shown.tags)) seed.add_tags = tag_set(shown.tags);

	seed.apply(xmp);
}

// The fields an edit writes outside the packet - EXIF and IPTC in a photo, ID3 frames or RIFF INFO in
// audio and video - in the same text the packet now holds, so the two always agree.
static libfiles::legacy_edits legacy_edits_of(const metadata_edits& edits, const libfiles::xmp::meta& xmp)
{
	libfiles::legacy_edits legacy;

	const auto items = [&xmp](const std::string_view ns, const std::string_view name)
	{
		std::vector<std::string> values;
		const auto count = xmp.count_array_items(ns, name);

		for (auto i = 1; i <= count; ++i)
		{
			std::string value;
			if (xmp.get_array_item(ns, name, i, &value)) values.push_back(std::move(value));
		}

		return values;
	};

	const auto text = [](const std::optional<std::string>& value) -> std::optional<std::string>
	{
		if (!value) return {};
		return str::utf8_cast2(*value);
	};

	legacy.title = text(edits.title);
	legacy.description = text(edits.description);
	legacy.copyright = text(edits.copyright_notice);
	legacy.city = text(edits.location_place);
	legacy.state = text(edits.location_state);
	legacy.country = text(edits.location_country);
	legacy.credit = text(edits.copyright_credit);
	legacy.source = text(edits.copyright_source);

	if (edits.copyright_creator) legacy.creators = items(xmp_ns::dc, "creator");
	if (!edits.add_tags.is_empty() || !edits.remove_tags.is_empty()) legacy.keywords = items(xmp_ns::dc, "subject");

	if (edits.rating)
	{
		const auto r = std::clamp(edits.rating.value(), -1, 5);
		legacy.rating = r;
		legacy.rating_percent = rating_to_percent(r);
	}

	if (edits.remove_rating)
	{
		legacy.rating = 0;
		legacy.rating_percent = 0;
	}

	if (edits.date_original) legacy.date_original = edits.date_original->to_xmp_date();
	if (edits.date_created) legacy.date_digitized = edits.date_created->to_xmp_date();

	if (edits.location_coordinate)
	{
		const auto position = edits.location_coordinate.value();
		legacy.gps_latitude = gps_coordinate::decimal_to_dms_str(position.latitude(), true);
		legacy.gps_longitude = gps_coordinate::decimal_to_dms_str(position.longitude(), false);
	}

	if (edits.orientation) legacy.orientation = static_cast<int>(edits.orientation.value());

	legacy.artist = text(edits.artist);
	legacy.album = text(edits.album);
	legacy.album_artist = text(edits.album_artist);
	legacy.genre = text(edits.genre);
	legacy.comment = text(edits.comment);
	legacy.copyright_url = text(edits.copyright_url);
	if (edits.track_num) legacy.track = str::utf8_cast2(edits.track_num->str());
	if (edits.disk_num) legacy.disc = str::utf8_cast2(edits.disk_num->str());

	// A bare year is the packet's Created date when no full date is given, as apply() writes it.
	if (edits.year && !edits.date_created) legacy.year = edits.year.value();

	return legacy;
}

// Patches the file where it lies when nothing it holds has to move - anything new is appended and only
// then referred to, so an interruption leaves a file that reads whole - otherwise writes the updated
// file beside it in one pass and swaps it in, so the file is never left half rewritten.
static void write_with_libfiles(const df::file_path path, const libfiles::metadata_update& update)
{
	auto f = open_file(path, platform::file_open_mode::read);
	if (!f) throw app_exception("the file could not be opened for reading");

	auto source = std::make_unique<platform_file_stream>(f);
	libfiles::reader r(*source);
	const auto plan = libfiles::plan_update(r, update);

	if (!plan.ok()) throw app_exception(plan.error);

	// Nothing the file holds moves.
	if (plan.patchable())
	{
		source.reset();
		f.reset();

		const auto rw = open_file(path, platform::file_open_mode::read_write);
		if (!rw) throw app_exception("the file could not be opened for writing");

		platform_file_stream file(rw);
		libfiles::write_in_place(file, plan);
		return;
	}

	const auto temp = platform::temp_file(path.extension(), path.folder());
	auto swapped = false;
	const df::scope_exit discard([temp, &swapped] { if (!swapped) platform::delete_file(temp); });

	{
		const auto out = open_file(temp, platform::file_open_mode::create);
		if (!out) throw app_exception("the updated file could not be created");

		platform_file_stream destination(out);
		libfiles::write_updated(r, plan, destination);
	}

	source.reset();
	f.reset();

	const auto replaced = platform::replace_file(path, temp, false);
	if (replaced.failed()) throw app_exception(replaced.format_error());
	swapped = true;
}

// The edit starts from the packet the file holds. libfiles reads it where it writes it; a format libfiles
// does not read - a HEIF or JPEG XL being converted - gives the packet diffractor's scan finds.
static void read_embedded_source(const df::file_path src_path, const file_type_ref src_ft, const bool converting,
                                 const metadata_edits& edits, libfiles::xmp::meta& xmp)
{
	std::optional<file_scan_result> scanned;

	const auto scan = [&]() -> const file_scan_result&
	{
		if (!scanned)
		{
			files ff;
			scanned = ff.scan_file(src_path, false, src_ft);
		}

		return *scanned;
	};

	// Without the source's packet the edit would start from an empty one and write away every
	// property the file already holds.
	if (const auto packet = read_file_packet(src_path))
	{
		if (!packet->readable) throw app_exception("the existing metadata could not be read");
		if (!packet->packet.empty()) xmp.parse(packet->packet);
	}
	else
	{
		const auto& source = scan();
		if (!source.success) throw app_exception("the existing metadata could not be read");
		if (!source.metadata.xmp.empty()) xmp.parse(packet_text(source.metadata.xmp));
	}

	// A converted file keeps what its source shows. The new format may have no place for the source's
	// IPTC, so what the packet lacks is taken from the item, as the toolkit's import of the source's
	// legacy metadata into the packet did.
	if (converting) seed_from_shown(*scan().to_props(), xmp);

	// Tags are edited relative to what the item shows. A packet without dc:subject leaves that to the
	// file's other keywords - IPTC, or the Windows categories and keywords of a video - so they become
	// the list the edit starts from. An empty dc:subject is a list of its own and stays one.
	if ((!edits.add_tags.is_empty() || !edits.remove_tags.is_empty()) && !xmp.get_property(xmp_ns::dc, "subject"))
	{
		const tag_set shown(scan().to_props()->tags);

		for (auto i = 0u; i < shown.size(); ++i)
		{
			xmp.append_array_item(xmp_ns::dc, "subject", libfiles::xmp::value_is_array, str::utf8_cast2(shown[i]));
		}
	}
}

// A failure the tests ask for, before anything is read. Action 2 first leaves a byte at the path being
// written, as an update that fails part way would.
static void run_test_update_failure(const df::file_path update_path)
{
	if (s_test_update_failure_sequence.empty()) return;

	const auto action = s_test_update_failure_sequence.front();
	s_test_update_failure_sequence.erase(s_test_update_failure_sequence.begin());
	if (action == 2)
	{
		std::ofstream f(platform::to_stream_path(update_path), std::ios::binary | std::ios::app);
		const char marker = 0;
		f.write(&marker, 1);
	}
	throw app_exception("test metadata update failure");
}

// The packet an edit starts from, with the edit applied: the one the source holds, its sidecar's, or,
// for a raw file without a sidecar, the one its scan finds.
static void read_edit_source(const df::file_path src_path, const file_type_ref src_ft, const bool converting,
                             const metadata_edits& edits, const std::string_view src_xmp_name,
                             libfiles::xmp::meta& xmp)
{
	if (src_ft->has_trait(file_traits::embedded_xmp))
	{
		read_embedded_source(src_path, src_ft, converting, edits, xmp);
	}
	else
	{
		const auto path_xmp = probe_xmp_path(src_path, src_xmp_name);
		const auto attributes = platform::file_attributes(path_xmp);

		// Only not_found proves there is no sidecar. Denied, offline, or an unreachable share all
		// answer unknown, and treating those as "no sidecar" is the data loss this guard exists to
		// stop, moved one step earlier.
		if (!attributes.confirmed_missing())
		{
			auto f = blob_from_file(path_xmp);

			// Same rule as the embedded branch: a sidecar that cannot be read is unknown, not empty.
			// Applying the edits to a default packet and swapping it over the file would write away
			// every property the sidecar already holds. An empty read is only believable when the file
			// is known to be zero length - and that has to be known, not merely reported by an
			// attribute query that may itself have failed.
			if (f.empty() && !(attributes.exists() && attributes.size == 0))
			{
				throw app_exception("the existing metadata could not be read");
			}

			if (!f.empty())
			{
				xmp.parse(std::string_view(std::bit_cast<const char*>(f.data()), f.size()));
			}
		}
		else if (src_ft->has_trait(file_traits::raw))
		{
			if (s_test_raw_source_xmp_path == src_path && !s_test_raw_source_xmp.empty())
			{
				xmp.parse(std::string_view(std::bit_cast<const char*>(s_test_raw_source_xmp.data()),
				                           s_test_raw_source_xmp.size()));
			}
			else
			{
				files ff;
				const auto scanned = ff.scan_file(src_path, false, src_ft);
				if (!scanned.success && !open_file(src_path, platform::file_open_mode::read))
				{
					throw app_exception("the existing metadata could not be read");
				}
				if (!scanned.metadata.xmp.empty())
				{
					xmp.parse(std::string_view(std::bit_cast<const char*>(scanned.metadata.xmp.data()),
					                           scanned.metadata.xmp.size()));
				}
			}
		}
	}

	edits.apply(xmp);
}

// A read or write failure, with what the system says about the file it happened to.
static app_exception file_failure(const std::exception& e, const df::file_path failing_path)
{
	const auto reason = platform::file_write_error(failing_path);
	return app_exception(reason.empty() ? std::string(e.what()) : std::format("{} ({})", reason, e.what()));
}

xmp_update_result metadata_xmp::update(const df::file_path update_path, const df::file_path src_path,
                                       const metadata_edits& edits, const std::string_view src_xmp_name,
                                       const df::file_path dst_xmp_path)
{
	xmp_update_result result;

	run_test_update_failure(update_path);

	// Which file a toolkit error refers to; the read source and the write target differ.
	auto failing_path = src_path;

	try
	{
		const auto* const src_ft = files::file_type_from_name(src_path);
		const auto* const dst_ft = files::file_type_from_name(update_path);
		const auto is_embedded_src = src_ft->has_trait(file_traits::embedded_xmp);
		const auto is_embedded_dst = dst_ft->has_trait(file_traits::embedded_xmp);

		const auto converting = is_embedded_src && is_embedded_dst &&
			libfiles_update_format(src_path) != libfiles_update_format(update_path);

		libfiles::xmp::meta xmp;
		read_edit_source(src_path, src_ft, converting, edits, src_xmp_name, xmp);

		failing_path = update_path;

		if (is_embedded_dst)
		{
			// A format diffractor reads a packet from but nothing writes one to.
			if (!libfiles::can_update(libfiles_update_format(update_path)))
			{
				throw app_exception("this file format cannot store the metadata");
			}

			libfiles::metadata_update update{&xmp};
			update.legacy = legacy_edits_of(edits, xmp);
			write_with_libfiles(update_path, update);
			result.success = true;
		}
		else
		{
			failing_path = dst_xmp_path;

			const auto buffer = xmp.serialize({.omit_packet_wrapper = true});

			const auto path_xmp = dst_xmp_path;
			const auto f = open_file(path_xmp, platform::file_open_mode::create);

			if (f)
			{
				const auto written = f->write(std::bit_cast<const uint8_t*>(buffer.data()), buffer.size());

				// A short write leaves a truncated sidecar. Reporting success here would present
				// a full disk or a dropped network share as a saved edit.
				if (written != buffer.size())
				{
					const auto reason = platform::file_write_error(path_xmp);
					throw app_exception(reason.empty() ? "the metadata sidecar could not be written" : reason);
				}

				result.success = true;
				result.xmp_path = path_xmp;
			}
			else
			{
				const auto reason = platform::file_write_error(path_xmp);
				throw app_exception(reason.empty() ? "the metadata sidecar could not be created" : reason);
			}
		}
	}
	catch (const libfiles::xmp::error& e)
	{
		// The packet itself refused the edit, such as a value for what the file holds as an array.
		df::log(__FUNCTION__, e.what());
		throw app_exception(e.what());
	}
	catch (const libfiles::read_error& e)
	{
		df::log(__FUNCTION__, e.what());
		throw file_failure(e, failing_path);
	}
	catch (const libfiles::write_error& e)
	{
		df::log(__FUNCTION__, e.what());
		throw file_failure(e, failing_path);
	}

	return result;
}

std::optional<xmp_update_result> metadata_xmp::stage_update(const df::file_path stage_path,
                                                            const df::file_path src_path,
                                                            const metadata_edits& edits,
                                                            const std::string_view src_xmp_name)
{
	const auto* const src_ft = files::file_type_from_name(src_path);
	if (!files::file_type_from_name(stage_path)->has_trait(file_traits::embedded_xmp)) return {};

	// Held until the stage is written. Windows lets no one write the file meanwhile, so the plan made
	// from it describes the bytes the stage is made from.
	const auto f = open_file(src_path, platform::file_open_mode::read);
	if (!f) return {};

	platform_file_stream source(f);
	libfiles::reader r(source);
	if (r.size() < 16) return {};

	uint8_t head[16];
	r.read(0, head, sizeof(head));
	if (!libfiles::can_update(libfiles::detect_file_format(head))) return {};

	run_test_update_failure(stage_path);

	xmp_update_result result;
	auto failing_path = src_path;

	try
	{
		libfiles::xmp::meta xmp;
		read_edit_source(src_path, src_ft, false, edits, src_xmp_name, xmp);

		libfiles::metadata_update update{&xmp};
		update.legacy = legacy_edits_of(edits, xmp);

		const auto plan = libfiles::plan_update(r, update);
		if (!plan.ok()) throw app_exception(plan.error);

		failing_path = stage_path;

		if (plan.patchable())
		{
			// The file system may copy without moving the bytes - a server-side copy on a share, a
			// block clone on ReFS - so a copy patched where it lies costs least.
			const auto copied = platform::copy_file(src_path, stage_path, true, false);
			if (copied.failed()) throw app_exception(copied.format_error());

			write_with_libfiles(stage_path, update);
		}
		else
		{
			const auto out = open_file(stage_path, platform::file_open_mode::create);
			if (!out) throw app_exception("the updated file could not be created");

			platform_file_stream destination(out);
			libfiles::write_updated(r, plan, destination);
		}

		result.success = true;
	}
	catch (const libfiles::xmp::error& e)
	{
		// The packet itself refused the edit, such as a value for what the file holds as an array.
		df::log(__FUNCTION__, e.what());
		throw app_exception(e.what());
	}
	catch (const libfiles::read_error& e)
	{
		df::log(__FUNCTION__, e.what());
		throw file_failure(e, failing_path);
	}
	catch (const libfiles::write_error& e)
	{
		df::log(__FUNCTION__, e.what());
		throw file_failure(e, failing_path);
	}

	return result;
}

bool metadata_xmp::patch(const df::file_path path, const metadata_edits& edits, const std::string_view src_xmp_name)
{
	run_test_update_failure(path);

	try
	{
		libfiles::xmp::meta xmp;
		read_edit_source(path, files::file_type_from_name(path), false, edits, src_xmp_name, xmp);

		libfiles::metadata_update update{&xmp};
		update.legacy = legacy_edits_of(edits, xmp);

		// One handle no one else may open meanwhile, for the plan and the patch: the bytes planned from
		// are the bytes patched.
		const auto f = open_file(path, platform::file_open_mode::read_write);
		if (!f) throw app_exception("the file could not be opened for writing");

		platform_file_stream file(f);
		libfiles::reader r(file);
		const auto plan = libfiles::plan_update(r, update);

		if (!plan.ok()) throw app_exception(plan.error);
		if (!plan.patchable()) return false;

		libfiles::write_in_place(file, plan);
		return true;
	}
	catch (const libfiles::xmp::error& e)
	{
		// The packet itself refused the edit, such as a value for what the file holds as an array.
		df::log(__FUNCTION__, e.what());
		throw app_exception(e.what());
	}
	catch (const libfiles::read_error& e)
	{
		df::log(__FUNCTION__, e.what());
		throw file_failure(e, path);
	}
	catch (const libfiles::write_error& e)
	{
		df::log(__FUNCTION__, e.what());
		throw file_failure(e, path);
	}
}

void metadata_xmp::update(std::string& buffer, const metadata_edits& edits)
{
	try
	{
		libfiles::xmp::meta meta;
		meta.parse(buffer);
		edits.apply(meta);
		buffer = str::utf8_cast(meta.serialize());
	}
	catch (const libfiles::xmp::error& e)
	{
		df::log(__FUNCTION__, e.what());
		throw app_exception(e.what());
	}
}

// The namespace URI is what the packet carries; the short name is what makes the section legible.
static std::string_view xmp_schema_title(const std::string_view ns)
{
	if (ns == "http://purl.org/dc/elements/1.1/") return "Dublin Core";
	if (ns == "http://ns.adobe.com/xap/1.0/") return "XMP Basic";
	if (ns == "http://ns.adobe.com/xap/1.0/rights/") return "XMP Rights";
	if (ns == "http://ns.adobe.com/xap/1.0/mm/") return "XMP Media Management";
	if (ns == "http://ns.adobe.com/xap/1.0/sType/ResourceEvent#") return "Resource Event";
	if (ns == "http://ns.adobe.com/xap/1.0/sType/ResourceRef#") return "Resource Reference";
	if (ns == "http://ns.adobe.com/photoshop/1.0/") return "Photoshop";
	if (ns == "http://ns.adobe.com/camera-raw-settings/1.0/") return "Camera Raw";
	if (ns == "http://ns.adobe.com/exif/1.0/") return "Exif";
	if (ns == "http://ns.adobe.com/exif/1.0/aux/") return "Exif Auxiliary";
	if (ns == "http://ns.adobe.com/tiff/1.0/") return "TIFF";
	if (ns == "http://iptc.org/std/Iptc4xmpCore/1.0/xmlns/") return "IPTC Core";
	if (ns == "http://iptc.org/std/Iptc4xmpExt/2008-02-29/") return "IPTC Extension";
	if (ns == "http://ns.adobe.com/xmp/1.0/DynamicMedia/") return "Dynamic Media";
	if (ns == "http://ns.microsoft.com/photo/1.0/") return "Microsoft Photo";
	if (ns == "http://ns.google.com/photos/1.0/camera/") return "Google Camera";
	return {};
}

// The pane draws the packet's own tree, and the iterator reports position only through the path,
// so depth is read back from the path's structure.
static int xmp_path_depth(const std::string_view path)
{
	auto depth = 0;
	for (const auto c : path) if (c == '/' || c == '[') ++depth;
	return depth;
}

static std::string xmp_shape(const libfiles::xmp::option_bits options, const int array_count)
{
	std::string result;

	if (libfiles::xmp::is_array(options))
	{
		if (libfiles::xmp::is_alt_text(options)) result = "Alt (language)";
		else if (libfiles::xmp::is_alternate(options)) result = "Alt";
		else if (libfiles::xmp::is_ordered(options)) result = "Seq";
		else result = "Bag";

		result += std::format(" ({})", array_count);
	}
	else if (libfiles::xmp::is_struct(options))
	{
		result = "Struct";
	}

	if (libfiles::xmp::is_qualifier_node(options))
	{
		if (!result.empty()) result += ", ";
		result += "qualifier";
	}

	return result;
}

metadata_kv_list metadata_xmp::to_info(const df::cspan xmp)
{
	bool parsed = false;
	return to_info(xmp, parsed);
}

metadata_kv_list metadata_xmp::to_info(const df::cspan xmp, bool& parsed)
{
	metadata_kv_list result;
	parsed = false;

	try
	{
		const auto text = packet_text(xmp);

		// A packet cut off part way parses without complaint and can come back empty, which is
		// indistinguishable from a complete packet that holds nothing - except that only the
		// complete one closes its RDF.
		const auto is_complete_packet = text.find("</rdf:RDF>") != std::string_view::npos;

		libfiles::xmp::meta meta;
		meta.parse(text);

		std::string current_schema;

		// Containers and empty properties are part of what the packet holds, so every node is
		// listed; the tree shape is carried on the row rather than used to filter it.
		for (const auto& node : meta.properties())
		{
			if (libfiles::xmp::is_schema(node.options) || node.path.empty()) continue;

			if (node.schema != current_schema)
			{
				current_schema = node.schema;
				const auto title = xmp_schema_title(node.schema);

				auto& section = result.emplace_back(title.empty() ? node.schema : std::string(title), std::string{});
				section.container = true;
				section.id = std::format("xmp.ns.{}", node.schema);
				if (!title.empty()) section.shape = node.schema;
			}

			const auto is_container = libfiles::xmp::is_array(node.options) || libfiles::xmp::is_struct(node.options);
			const auto array_count = libfiles::xmp::is_array(node.options)
				                         ? meta.count_array_items(node.schema, node.path)
				                         : 0;

			auto& row = result.emplace_back(node.path, str::utf8_cast(node.value));
			row.depth = xmp_path_depth(node.path) + 1;
			row.container = is_container;
			row.id = std::format("xmp.{}", node.path);

			const auto shape = xmp_shape(node.options, array_count);
			if (!shape.empty()) row.shape = shape;

			// A long value is kept whole behind the row rather than trimmed away.
			constexpr auto inline_value_limit = 200_z;

			if (row.value.size() > inline_value_limit)
			{
				row.detail = metadata_text_detail{row.value};
				row.value = row.value.substr(0, inline_value_limit) + "\xE2\x80\xA6";
			}
		}

		parsed = !result.empty() || is_complete_packet;
	}
	catch (const std::exception& e)
	{
		record_xmp_error(__FUNCTION__, e.what());
	}

	return result;
}
