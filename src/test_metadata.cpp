// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Metadata tests (metadata_exif, metadata_iptc, metadata_xmp, metadata_icc) -- scanning
// properties from JPEG, TIFF, MP3, MP4, MOV, MKV, AVI, RAW, HEIF, AVIF, WebP, JXL and MOD files, tag
// editing and removal, sidecar merging, metadata presentation blocks and filename token replacement.

#include "pch.h"

#include "metadata_xmp.h"
#include "metadata_exif.h"
#include "metadata_icc.h"
#include "metadata_iptc.h"
#include "test_fixtures.h"
#include "test_runner.h"
#include "av_format.h"
#include "av_player.h"
#include "libfiles/reader.h"
#include "libfiles/xmp.h"

#include <zlib.h>
#include <jxl/encode.h>
#include <jxl/color_encoding.h>

#define LIBHEIF_STATIC_BUILD 1
#include <libheif/heif.h>
#include <libheif/heif_items.h>

static void should_replace_tokens()
{
	files ff;
	const auto image_md = ff_scan_file(ff, test_files_folder.combine_file("Test.jpg"));
	const auto audio_md = ff_scan_file(ff, test_files_folder.combine_file("Colorblind.mp3"));

	const auto md1 = image_md.to_props();
	const auto md2 = audio_md.to_props();

	assert_equal("2012\\2012-09-14", replace_tokens("{year}\\{created}"s, md1, {}, md1->created()));
	assert_equal("2012\\Test.jpg", replace_tokens("{year}\\{name}"s, md1, "Test.jpg", md1->created()));
	assert_equal("2012\\09\\14", replace_tokens("{year}\\{month}\\{day}"s, md1, {}, md1->created()));
	assert_equal("2012\\september\\14", replace_tokens("{year}\\{month.text}\\{day}"s, md1, {}, md1->created()));
	assert_equal("2012\\SEP\\14", replace_tokens("{year}\\{month.short}\\{day}"s, md1, {}, md1->created()));
	assert_equal("Counting Crows\\This Desert Life",
	             replace_tokens("{artist}\\{album}"s, md2, {}, md2->created()));
}

static void should_scan_jpeg()
{
	const auto load_path = test_files_folder.combine_file("Test.jpg");

	prop::item_metadata expected_exif;
	expected_exif.camera_manufacturer = "Canon"_c;
	expected_exif.camera_model = "Canon EOS 7D"_c;
	expected_exif.lens = "EF-S15-85mm f/3.5-5.6 IS USM"_c;
	expected_exif.description = "Caption"_c;
	expected_exif.coordinate = gps_coordinate(50.08806, 14.42083);
	expected_exif.copyright_notice = "Copyright"_c;
	expected_exif.f_number = 6.3f;
	expected_exif.exposure_time = 1.0f / 100.0f;
	expected_exif.iso_speed = 100;
	expected_exif.focal_length = 15.0f;
	expected_exif.dates.add(prop::date_source::exif_digitized, df::date_t(2012, 9, 14, 19, 21, 14));
	expected_exif.dates.add(prop::date_source::exif_original, df::date_t(2012, 9, 14, 19, 21, 14));
	expected_exif.dates.add(prop::date_source::exif_datetime, df::date_t(2012, 9, 14, 19, 21, 14));
	expected_exif.rating = 0;

	assert_metadata(expected_exif, *extract_properties(load_path, metadata_type::EXIF), "EXIF");

	prop::item_metadata expected_iptc;
	expected_iptc.title = "Title"_c;
	expected_iptc.description = "Caption"_c;
	expected_iptc.tags = "key1 key2 key3"_c;
	expected_iptc.location_place = "Prague"_c;
	expected_iptc.location_state = "Hlavní Mesto Praha"_c;
	expected_iptc.location_country = "Czech Republic"_c;
	expected_iptc.copyright_notice = "Copyright"_c;
	expected_iptc.rating = 0;
	// 2:55 and 2:60, which agree with the EXIF original this file also carries.
	expected_iptc.dates.add(prop::date_source::iptc_created, df::date_t(2012, 9, 14, 19, 21, 14));

	assert_metadata(expected_iptc, *extract_properties(load_path, metadata_type::IPTC), "IPTC");

	const auto actual_xmp = extract_properties(load_path, metadata_type::XMP);

	prop::item_metadata expected_xmp;
	expected_xmp.title = "Title"_c;
	expected_xmp.description = "Caption"_c;
	expected_xmp.tags = "key1 key2 key3"_c;
	expected_xmp.location_place = "Prague"_c;
	expected_xmp.location_state = "Hlavní Mesto Praha"_c;
	expected_xmp.location_country = "Czech Republic"_c;
	expected_xmp.lens = "EF-S15-85mm f/3.5-5.6 IS USM"_c;
	expected_xmp.copyright_notice = "Copyright"_c;
	expected_xmp.rating = 4;
	expected_xmp.dates.add(prop::date_source::xmp_exif_digitized, df::date_t(2012, 9, 14, 19, 21, 14));
	expected_xmp.dates.add(prop::date_source::xmp_exif_original, df::date_t(2012, 9, 14, 19, 21, 14));
	expected_xmp.dates.add(prop::date_source::xmp_modify, df::date_t(2012, 9, 14, 19, 21, 14));

	assert_metadata(expected_xmp, *actual_xmp, "XMP");

	const auto actual_all = extract_properties(load_path, metadata_type::ALL);
	assert_metadata(*expected_test_jpg(), *actual_all, "all");
}

static void should_parse_xmp()
{
	const auto load_path = test_files_folder.combine_file("IMG_0604.xmp");

	prop::item_metadata actual;
	metadata_xmp::parse(actual, load_path);

	assert_equal("Flower", actual.title);
	assert_equal("Blomst", actual.tags);
	assert_equal("Følfod ( Tussilago farfara ) Lægeurt", actual.description);
	assert_equal("Frank Aalestrup www.fdaa.dk", actual.copyright_notice);
	assert_equal("\"Frank Aalestrup.\nwww.fdaa.dk\"", actual.copyright_creator);
	assert_equal("EF100mm f/2.8L Macro IS USM", actual.lens);
	assert_equal({56.19283, 9.88415}, actual.coordinate);
	assert_equal("Canon", actual.camera_manufacturer);
	assert_equal("Canon EOS 50D", actual.camera_model);
	assert_equal("IMG_0604.CR2", actual.raw_file_name);
	assert_equal("Denmark", actual.location_country);
	assert_equal(false, actual.is_panorama(), "an ordinary photograph declares no panorama");
}

// XMP is parsed last, so an array that is simply absent must not erase what EXIF or IPTC supplied.
// An empty array is a different statement: it is the file saying it has no tags, and it is exactly
// what the writer emits when the user clears them. Reading only the non-empty case meant clearing
// every tag and rescanning brought the legacy keywords straight back.
static void should_clear_tags_from_an_empty_xmp_subject()
{
	const auto write_sidecar = [](const std::string_view dc_body)
	{
		const auto path = _temps.next_path(".xmp");
		const auto xml = std::format(
			"<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
			"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
			"xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
			"<rdf:Description rdf:about=\"\" "
			"xmlns:dc=\"http://purl.org/dc/elements/1.1/\">{}</rdf:Description>"
			"</rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>",
			dc_body);

		std::ofstream f(platform::to_stream_path(path), std::ios::binary | std::ios::trunc);
		f.write(xml.data(), static_cast<std::streamsize>(xml.size()));
		f.close();
		return path;
	};

	{
		// What the writer produces for an item whose tags the user removed.
		prop::item_metadata md;
		md.tags = "Holiday Portugal"_c;
		metadata_xmp::parse(md, write_sidecar("<dc:subject><rdf:Bag/></dc:subject>"));
		assert_equal(true, str::is_empty(md.tags), "an empty subject array clears the tags it supersedes");
	}

	{
		// No dc:subject at all says nothing about tags, so what EXIF or IPTC gave must survive.
		prop::item_metadata md;
		md.tags = "Holiday Portugal"_c;
		metadata_xmp::parse(md, write_sidecar("<dc:title>Lisbon</dc:title>"));
		assert_equal("Holiday Portugal", md.tags, "an absent subject array leaves them alone");
	}

	{
		prop::item_metadata md;
		md.tags = "Holiday Portugal"_c;
		metadata_xmp::parse(md, write_sidecar(
			                    "<dc:subject><rdf:Bag><rdf:li>Lisbon</rdf:li></rdf:Bag></dc:subject>"));
		assert_equal("Lisbon", md.tags, "and a populated one replaces them");
	}
}

// XMP GPS is applied as a pair. A sidecar carrying only one component used to overwrite that half
// while the other stayed at whatever EXIF had already put there, pinning the item to a coordinate
// neither source states - and an item with no EXIF coordinate at all got the zero half, which is
// the Gulf of Guinea.
static void should_apply_xmp_gps_as_a_pair()
{
	const auto write_sidecar = [](const std::string_view exif_body)
	{
		const auto path = _temps.next_path(".xmp");
		const auto xml = std::format(
			"<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
			"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
			"xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
			"<rdf:Description rdf:about=\"\" "
			"xmlns:exif=\"http://ns.adobe.com/exif/1.0/\">{}</rdf:Description>"
			"</rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>",
			exif_body);

		std::ofstream f(platform::to_stream_path(path), std::ios::binary | std::ios::trunc);
		f.write(xml.data(), static_cast<std::streamsize>(xml.size()));
		f.close();
		return path;
	};

	{
		prop::item_metadata md;
		metadata_xmp::parse(md, write_sidecar("<exif:GPSLatitude>51,30.852N</exif:GPSLatitude>"
			"<exif:GPSLongitude>0,5.910W</exif:GPSLongitude>"));
		assert_equal(true, md.coordinate.is_valid(), "a complete pair is read");
		assert_equal(true, std::abs(md.coordinate.latitude() - 51.5142) < 0.001, "latitude");
		assert_equal(true, std::abs(md.coordinate.longitude() + 0.0985) < 0.001, "longitude");
	}

	{
		// The item already carries an EXIF coordinate, as a scanned photo with a sidecar would.
		prop::item_metadata md;
		md.coordinate = gps_coordinate(35.68, 139.69);
		metadata_xmp::parse(md, write_sidecar("<exif:GPSLatitude>51,30.852N</exif:GPSLatitude>"));

		assert_equal(true, std::abs(md.coordinate.latitude() - 35.68) < 0.001,
		             "half a pair changes neither component");
		assert_equal(true, std::abs(md.coordinate.longitude() - 139.69) < 0.001,
		             "so the EXIF coordinate is left whole");
	}

	{
		prop::item_metadata md;
		metadata_xmp::parse(md, write_sidecar("<exif:GPSLongitude>0,5.910W</exif:GPSLongitude>"));
		assert_equal(false, md.coordinate.is_valid(), "half a pair alone locates nothing");
	}

	const auto assert_rejected = [&write_sidecar](const std::string_view latitude, const std::string_view longitude,
	                                             const std::string_view message)
	{
		prop::item_metadata md;
		md.coordinate = gps_coordinate(35.68, 139.69);
		metadata_xmp::parse(md, write_sidecar(std::format("<exif:GPSLatitude>{}</exif:GPSLatitude>"
			"<exif:GPSLongitude>{}</exif:GPSLongitude>", latitude, longitude)));
		assert_equal(gps_coordinate(35.68, 139.69), md.coordinate, message);
	};

	assert_rejected("91,0N", "0,5.910W", "invalid latitude keeps the existing coordinate");
	assert_rejected("51,30.852E", "0,5.910W", "wrong latitude hemisphere keeps the existing coordinate");
	assert_rejected("51,30.852X", "0,5.910W", "unknown latitude hemisphere keeps the existing coordinate");
	assert_rejected("51,30.852N trailing", "0,5.910W", "trailing latitude junk keeps the existing coordinate");
	assert_rejected("51,60.0N", "0,5.910W", "invalid latitude minutes keeps the existing coordinate");
	assert_rejected("51,30,60N", "0,5.910W", "invalid latitude seconds keeps the existing coordinate");
	assert_rejected("51,30.852N", "180,0E", "invalid longitude keeps the existing coordinate");
}

static void should_read_xmp_disc_number()
{
	const auto write_sidecar = [](const std::string_view dm_body)
	{
		const auto path = _temps.next_path(".xmp");
		const auto xml = std::format(
			"<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
			"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
			"xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
			"<rdf:Description rdf:about=\"\" "
			"xmlns:xmpDM=\"http://ns.adobe.com/xmp/1.0/DynamicMedia/\">{}</rdf:Description>"
			"</rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>",
			dm_body);

		std::ofstream f(platform::to_stream_path(path), std::ios::binary | std::ios::trunc);
		f.write(xml.data(), static_cast<std::streamsize>(xml.size()));
		f.close();
		return path;
	};

	{
		prop::item_metadata md;
		metadata_xmp::parse(md, write_sidecar("<xmpDM:discNumber>2/3</xmpDM:discNumber>"));
		assert_equal(2, static_cast<int>(md.disk.x), "disc number");
		assert_equal(3, static_cast<int>(md.disk.y), "disc total");
	}

	{
		prop::item_metadata md;
		md.disk = {1, 4};
		metadata_xmp::parse(md, write_sidecar("<xmpDM:discNumber>2/3</xmpDM:discNumber>"));
		assert_equal(2, static_cast<int>(md.disk.x), "xmp disc number replaces lower priority value");
		assert_equal(3, static_cast<int>(md.disk.y), "xmp disc total replaces lower priority value");
	}

	{
		prop::item_metadata md;
		metadata_xmp::parse(md, write_sidecar("<xmpDM:discNumber>2</xmpDM:discNumber>"));
		assert_equal(2, static_cast<int>(md.disk.x), "single disc number is parsed");
		assert_equal(0, static_cast<int>(md.disk.y), "single disc total remains unspecified");
	}
}

// Rotating a photo writes a well-formed packet with nothing left in it, and the panel called that
// "not understood". Only a packet the toolkit cannot read is unparsed.
static void should_report_empty_xmp_packet_as_parsed()
{
	const std::string empty_packet =
		R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
		R"(<rdf:Description rdf:about=""/></rdf:RDF></x:xmpmeta>)";
	bool parsed = false;
	const auto empty_rows = metadata_xmp::to_info(df::cspan{std::bit_cast<const uint8_t*>(empty_packet.data()),
	                                                        empty_packet.size()}, parsed);
	assert_equal(true, empty_rows.empty(), "an empty packet has no rows");
	assert_equal(true, parsed, "an empty packet is understood");

	const std::string broken_packet = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF><rdf:Description)";
	metadata_xmp::to_info(df::cspan{std::bit_cast<const uint8_t*>(broken_packet.data()), broken_packet.size()},
	                      parsed);
	assert_equal(false, parsed, "a malformed packet is not understood");
}

// The flag records what the file says about itself, not what its shape suggests: a 2:1 crop of a
// landscape is not a panorama, and a photo sphere that has been cropped square still is one.
static void should_read_the_declared_panorama_projection()
{
	const auto write_sidecar = [](const std::string_view gpano_body)
	{
		const auto path = _temps.next_path(".xmp");
		const auto xml = std::format(
			"<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
			"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
			"xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
			"<rdf:Description rdf:about=\"\" "
			"xmlns:GPano=\"http://ns.google.com/photos/1.0/panorama/\">{}</rdf:Description>"
			"</rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>",
			gpano_body);

		std::ofstream f(platform::to_stream_path(path), std::ios::binary | std::ios::trunc);
		f.write(xml.data(), static_cast<std::streamsize>(xml.size()));
		f.close();

		prop::item_metadata md;
		metadata_xmp::parse(md, path);
		return md.panorama;
	};

	assert_equal(static_cast<int>(prop::panorama_projection::equirectangular),
	             static_cast<int>(write_sidecar("<GPano:ProjectionType>equirectangular</GPano:ProjectionType>")),
	             "a photo sphere");
	assert_equal(static_cast<int>(prop::panorama_projection::cylindrical),
	             static_cast<int>(write_sidecar("<GPano:ProjectionType>cylindrical</GPano:ProjectionType>")),
	             "a cylindrical stitch");

	// A projection this build does not know still answers the question the flag exists to answer.
	assert_equal(static_cast<int>(prop::panorama_projection::unspecified),
	             static_cast<int>(write_sidecar("<GPano:ProjectionType>mercator</GPano:ProjectionType>")),
	             "an unrecognised projection is still a panorama");

	// Writers that declare a panorama without naming its projection must not be read as ordinary.
	assert_equal(static_cast<int>(prop::panorama_projection::unspecified),
	             static_cast<int>(write_sidecar("<GPano:UsePanoramaViewer>True</GPano:UsePanoramaViewer>")),
	             "declared without a projection");
	assert_equal(static_cast<int>(prop::panorama_projection::unspecified),
	             static_cast<int>(write_sidecar("<GPano:FullPanoWidthPixels>8192</GPano:FullPanoWidthPixels>")),
	             "declared by its full extent");

	assert_equal(static_cast<int>(prop::panorama_projection::none),
	             static_cast<int>(write_sidecar("")),
	             "and a file carrying no GPano at all is not a panorama");

	// The presence bit is what lets the index reject candidates before it loads them.
	prop::item_metadata pano;
	pano.panorama = prop::panorama_projection::equirectangular;
	assert_equal(true, (pano.calc_search_presence().types & search_presence_mask::panorama) != 0,
	             "a panorama carries the presence bit");

	constexpr prop::item_metadata plain;
	assert_equal(false, (plain.calc_search_presence().types & search_presence_mask::panorama) != 0,
	             "and an ordinary file does not");
}

static void should_scan_tiff_camera_metadata()
{
	libfiles::exif_fields fields;
	fields.camera_make = "Canon";
	fields.camera_model = "EOS R5";
	fields.lens = "RF 50mm F1.8";
	fields.f_number = 2.8;
	fields.exposure_time = 1.0 / 250;
	fields.iso_speed = 400;
	fields.original = "2024:05:06 07:08:00";
	fields.digitized = "2024:05:06 07:08:01";
	fields.modified = "2024:05:06 07:08:09";
	fields.orientation = 6;
	auto bytes = libfiles::make_exif(fields);
	libfiles::reader reader(bytes);
	const auto ifd0 = reader.le32(4);
	const auto count = reader.le16(ifd0);
	auto dimensions = 0;
	for (auto i = 0u; i < count; ++i)
	{
		const auto at = ifd0 + 2u + i * 12u;
		const auto tag = reader.le16(at);
		if (tag != 0x011a && tag != 0x011b) continue;
		// Replace the writer's resolution entries with a TIFF image's dimensions.
		const uint8_t entry[] = {static_cast<uint8_t>(tag == 0x011a ? 0x00 : 0x01), 0x01,
			0x03, 0, 1, 0, 0, 0, static_cast<uint8_t>(tag == 0x011a ? 64 : 32), 0, 0, 0};
		std::copy(std::begin(entry), std::end(entry), bytes.begin() + at);
		++dimensions;
	}
	assert_equal(2, dimensions, "the TIFF fixture declares width and height");

	mem_read_stream stream({bytes.data(), bytes.size()});
	auto scanned = scan_photo(stream, scan_intent::index, false);
	assert_equal(true, scanned.success, "TIFF metadata is scanned without decoding pixels");
	assert_equal(true, scanned.metadata.exif.empty(), "the image is not copied into an EXIF block");
	const auto md = scanned.to_props();
	assert_equal("Canon"sv, md->camera_manufacturer.sv(), "TIFF camera make");
	assert_equal("EOS R5"sv, md->camera_model.sv(), "TIFF camera model");
	assert_equal("RF 50mm F1.8"sv, md->lens.sv(), "TIFF lens");
	assert_equal(400, static_cast<int>(md->iso_speed), "TIFF ISO");
	assert_equal(true, std::abs(md->f_number - 2.8f) < 0.0001f, "TIFF aperture");
	assert_equal(true, std::abs(md->exposure_time - 1.0f / 250) < 0.000001f, "TIFF exposure");
	assert_equal(df::date_t(2024, 5, 6, 7, 8, 0), md->dates.original(), "TIFF capture date");
	assert_equal(df::date_t(2024, 5, 6, 7, 8, 1), md->dates.created(), "TIFF digitized date");
	assert_equal(df::date_t(2024, 5, 6, 7, 8, 9), md->dates.modified(), "TIFF modified date");
	assert_equal(ui::orientation::right_top, md->orientation, "TIFF orientation");

	libfiles::xmp::meta xmp;
	xmp.set_property(libfiles::xmp::ns::tiff, "Model", "XMP camera");
	const auto packet = xmp.serialize();
	scanned.metadata.xmp.assign(packet.begin(), packet.end());
	assert_equal("XMP camera"sv, scanned.to_props()->camera_model.sv(), "XMP still overrides native TIFF fields");
}

static std::string panorama_packet(const prop::panorama_geometry& geometry)
{
	return std::format(
		"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
		"xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
		"<rdf:Description rdf:about=\"\" xmlns:GPano=\"http://ns.google.com/photos/1.0/panorama/\" "
		"GPano:ProjectionType=\"equirectangular\" GPano:FullPanoWidthPixels=\"{}\" "
		"GPano:FullPanoHeightPixels=\"{}\" GPano:CroppedAreaLeftPixels=\"{}\" "
		"GPano:CroppedAreaTopPixels=\"{}\" GPano:CroppedAreaImageWidthPixels=\"{}\" "
		"GPano:CroppedAreaImageHeightPixels=\"{}\"/></rdf:RDF></x:xmpmeta>",
		geometry.full_width, geometry.full_height, geometry.cropped_left, geometry.cropped_top,
		geometry.cropped_width, geometry.cropped_height);
}

static df::file_path write_panorama_fixture(const bool heif, const std::string_view packet, const bool compress)
{
	const auto path = _temps.next_path(heif ? ".heif" : ".jxl");
	if (heif)
	{
		const std::unique_ptr<heif_context, decltype(&heif_context_free)> context(heif_context_alloc(), heif_context_free);
		assert_equal(true, context != nullptr, "HEIF context created");
		heif_image* raw_image = nullptr;
		assert_equal(heif_error_Ok, heif_image_create(4, 2, heif_colorspace_RGB, heif_chroma_interleaved_RGB,
		                                            &raw_image).code, "HEIF image created");
		const std::unique_ptr<heif_image, decltype(&heif_image_release)> image(raw_image, heif_image_release);
		assert_equal(heif_error_Ok, heif_image_add_plane(image.get(), heif_channel_interleaved, 4, 2, 8).code,
		             "HEIF plane allocated");
		int stride = 0;
		auto* const plane = heif_image_get_plane(image.get(), heif_channel_interleaved, &stride);
		assert_equal(true, plane != nullptr && stride >= 12, "HEIF plane is large enough");
		std::memset(plane, 64, static_cast<size_t>(stride) * 2);
		heif_encoder* raw_encoder = nullptr;
		assert_equal(heif_error_Ok, heif_context_get_encoder_for_format(context.get(), heif_compression_uncompressed,
		                                                               &raw_encoder).code, "HEIF encoder available");
		const std::unique_ptr<heif_encoder, decltype(&heif_encoder_release)> encoder(raw_encoder, heif_encoder_release);
		heif_image_handle* raw_handle = nullptr;
		assert_equal(heif_error_Ok, heif_context_encode_image(context.get(), image.get(), encoder.get(), nullptr,
		                                                    &raw_handle).code, "HEIF fixture encoded");
		const std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> handle(
			raw_handle, heif_image_handle_release);
		assert_equal(heif_error_Ok, heif_context_add_XMP_metadata(context.get(), handle.get(), packet.data(),
		                                                        static_cast<int>(packet.size())).code, "GPano embedded in HEIF");
		assert_equal(heif_error_Ok, heif_context_write_to_file(context.get(), path.str().c_str()).code,
		             "HEIF fixture written");
	}
	else
	{
		const std::unique_ptr<JxlEncoder, decltype(&JxlEncoderDestroy)> encoder(JxlEncoderCreate(nullptr), JxlEncoderDestroy);
		assert_equal(true, encoder != nullptr, "JPEG XL encoder created");
		JxlBasicInfo info{};
		JxlEncoderInitBasicInfo(&info);
		info.xsize = 4;
		info.ysize = 2;
		info.bits_per_sample = 8;
		info.uses_original_profile = JXL_TRUE;
		info.num_color_channels = 3;
		assert_equal(JXL_ENC_SUCCESS, JxlEncoderSetBasicInfo(encoder.get(), &info), "JPEG XL geometry accepted");
		JxlColorEncoding color{};
		JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
		assert_equal(JXL_ENC_SUCCESS, JxlEncoderSetColorEncoding(encoder.get(), &color), "JPEG XL colour accepted");
		assert_equal(JXL_ENC_SUCCESS, JxlEncoderUseContainer(encoder.get(), JXL_TRUE), "JPEG XL container enabled");
		assert_equal(JXL_ENC_SUCCESS, JxlEncoderUseBoxes(encoder.get()), "JPEG XL metadata enabled");
		assert_equal(JXL_ENC_SUCCESS, JxlEncoderAddBox(encoder.get(), "xml ",
			reinterpret_cast<const uint8_t*>(packet.data()), packet.size(), compress ? JXL_TRUE : JXL_FALSE),
			"GPano embedded in JPEG XL");
		auto* const settings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
		assert_equal(true, settings != nullptr, "JPEG XL frame settings created");
		assert_equal(JXL_ENC_SUCCESS, JxlEncoderSetFrameLossless(settings, JXL_TRUE), "lossless JPEG XL fixture");
		assert_equal(JXL_ENC_SUCCESS, JxlEncoderFrameSettingsSetOption(settings, JXL_ENC_FRAME_SETTING_EFFORT, 1),
		             "fast fixture encoding");
		const JxlPixelFormat format{3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
		const std::array<uint8_t, 24> pixels{};
		assert_equal(JXL_ENC_SUCCESS, JxlEncoderAddImageFrame(settings, &format, pixels.data(), pixels.size()),
		             "JPEG XL pixels accepted");
		JxlEncoderCloseInput(encoder.get());
		std::array<uint8_t, 16384> encoded{};
		auto* next = encoded.data();
		auto available = encoded.size();
		assert_equal(JXL_ENC_SUCCESS, JxlEncoderProcessOutput(encoder.get(), &next, &available), "JPEG XL fixture encoded");
		const auto size = encoded.size() - available;
		const std::string_view contents(reinterpret_cast<const char*>(encoded.data()), size);
		assert_equal(compress, contents.find("brob") != std::string_view::npos, "the fixture exercises compressed boxes");
		std::ofstream file(platform::to_stream_path(path), std::ios::binary);
		file.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(size));
		file.close();
		assert_equal(true, file.good(), "JPEG XL fixture written");
	}
	return path;
}

static void should_read_embedded_panorama_geometry()
{
	const prop::panorama_geometry embedded{8, 4, 2, 1, 4, 2};
	const prop::panorama_geometry sidecar{12, 6, 0, 0, 4, 2};
	for (const auto heif : {true, false})
	{
		for (const auto compress : {false, true})
		{
			if (heif && compress) continue;
			const auto path = write_panorama_fixture(heif, panorama_packet(embedded), compress);
			assert_equal(true, metadata_xmp::panorama(path) == embedded, "embedded HEIF/JPEG XL GPano geometry");
			const auto sidecar_path = path.extension(".xmp");
			std::ofstream file(platform::to_stream_path(sidecar_path), std::ios::binary);
			const auto packet = panorama_packet(sidecar);
			file.write(packet.data(), static_cast<std::streamsize>(packet.size()));
			file.close();
			assert_equal(true, file.good(), "sidecar fixture written");
			assert_equal(true, metadata_xmp::panorama(path) == embedded, "valid embedded geometry keeps precedence");

			auto incomplete = embedded;
			incomplete.full_height = 0;
			const auto fallback_path = write_panorama_fixture(heif, panorama_packet(incomplete), compress);
			assert_equal(false, metadata_xmp::panorama(fallback_path).is_valid(), "incomplete geometry is not projected");
			const auto fallback_sidecar = fallback_path.extension(".xmp");
			std::ofstream fallback(platform::to_stream_path(fallback_sidecar), std::ios::binary);
			fallback.write(packet.data(), static_cast<std::streamsize>(packet.size()));
			fallback.close();
			assert_equal(true, fallback.good(), "fallback sidecar written");
			assert_equal(true, metadata_xmp::panorama(fallback_path) == sidecar,
			             "the sidecar replaces the incomplete embedded declaration as a whole");
		}
	}
}

static void should_merge_xmp_sidecar()
{
	const auto cr2_path = test_files_folder.combine_file("Gherkin.CR2");
	const auto xmp_path = test_files_folder.combine_file("Gherkin.xmp");

	const auto actual = extract_properties(cr2_path);

	metadata_xmp::parse(*actual, xmp_path);

	assert_equal("Canon", actual->camera_manufacturer);
	assert_equal("United Kingdom", actual->location_country);
	assert_equal("© Mark Ridgwell", actual->copyright_notice);
	assert_equal("\"Mark Ridgwell\"", actual->copyright_creator);
}

static void should_scan_mp3()
{
	const auto load_path = test_files_folder.combine_file("Colorblind.mp3");

	files ff;
	const auto actual = ff_scan_file(ff, load_path);

	prop::item_metadata expected;
	expected.artist = "Counting Crows"_c;
	expected.album_artist = "Counting Crows"_c;
	expected.title = "Colorblind"_c;
	expected.album = "This Desert Life"_c;
	expected.comment = "Comments"_c;
	expected.composer = "Adam Duritz/Charlie Gillingham"_c;
	expected.publisher = "Interscope"_c;
	expected.rating = 5;
	expected.genre = "Rock"_c;
	expected.duration = 10;
	expected.audio_sample_rate = 22050;
	expected.audio_channels = 2;
	expected.audio_codec = "mp3"_c;
	expected.track.x = 7;
	expected.year = 1999;

	assert_metadata(expected, *actual.to_props(), "Colorblind.mp3");

	const auto load_path2 = test_files_folder.combine_file("Games Without Frontiers.mp3");
	const auto actual2 = ff_scan_file(ff, load_path2);

	prop::item_metadata expected2;
	expected2.artist = "Peter Gabriel"_c;
	expected2.album = "Hit"_c;
	expected2.title = "Games Without Frontiers"_c;
	expected2.year = 2003;
	expected2.genre = "Rock"_c;
	expected2.duration = 10;
	expected2.audio_sample_rate = 44100;
	expected2.audio_channels = 2;
	expected2.track.x = 5;
	expected2.disk.x = 1;
	expected2.encoder = "Lavf51.12.1"_c;
	expected2.audio_codec = "mp3"_c;

	assert_metadata(expected2, *actual2.to_props(), "Games Without Frontiers.mp3");

	const auto load_path3 = test_files_folder.combine_file("Is It Any Wonder.mp3");
	const auto actual3 = ff_scan_file(ff, load_path3);

	prop::item_metadata expected3;
	expected3.artist = "Keane"_c;
	expected3.album = "Under The Iron Sea"_c;
	expected3.title = "Is It Any Wonder?"_c;
	expected3.year = 2006;
	expected3.genre = "Rock"_c;
	expected3.duration = 10;
	expected3.audio_sample_rate = 44100;
	expected3.audio_channels = 2;
	expected3.track.x = 2;
	expected3.audio_codec = "mp3"_c;
	// TDRC "2006-06-20" names a calendar day, not an instant, so it is the day wherever it is shown.
	expected3.dates.add(prop::date_source::container_created, df::date_t(2006, 6, 20, 0, 0, 0));
	expected3.publisher = "Interscope"_c;

	assert_metadata(expected3, *actual3.to_props(), "Is It Any Wonder.mp3");
}

static void should_scan_mp4()
{
	const auto load_path = test_files_folder.combine_file("gizmo.mp4");

	files ff;
	const auto actual = ff_scan_file(ff, load_path);

	prop::item_metadata expected;
	expected.title = "Title xxx"_c;
	expected.comment = "Comments xxx"_c;
	expected.tags = "gadget test"_c;
	expected.audio_codec = "aac"_c;
	expected.audio_sample_rate = 48000;
	expected.audio_channels = 1;
	expected.duration = 6;
	expected.width = 560;
	expected.height = 320;
	expected.rating = 4;
	expected.dates.add_utc(prop::date_source::container_created, df::date_t(2010, 3, 20, 21, 29, 11));
	expected.dates.add_utc(prop::date_source::xmp_modify, df::date_t(2010, 3, 20, 21, 29, 12));
	expected.year = 2010;
	expected.video_codec = "h264"_c;
	expected.encoder = "HandBrake 0.9.4 2009112300"_c;
	expected.pixel_format = "yuv420"_c;

	assert_metadata(expected, *actual.to_props(), "gizmo.mp4");

	const auto load_path2 = test_files_folder.combine_file("This Year's Love.m4a");
	const auto actual2 = ff_scan_file(ff, load_path2);

	prop::item_metadata expected2;
	expected2.title = "This Year's Love"_c;
	expected2.artist = "David Gray"_c;
	expected2.album_artist = "David Gray"_c;
	expected2.composer = "David Gray"_c;
	expected2.album = "David Gray: Greatest Hits"_c;
	expected2.copyright_notice = "\u2117 2007 Iht Records Ltd under exclusive licence to Warner Music UK Ltd"_c;
	expected2.dates.add_utc(prop::date_source::container_created, df::date_t(2007, 11, 9, 8, 0, 0));
	expected2.genre = "Pop"_c;
	expected2.duration = 10;
	expected2.audio_codec = "aac"_c;
	expected2.audio_sample_rate = 44100;
	expected2.audio_channels = 2;
	expected2.track = {7, 0};
	expected2.width = 0;
	expected2.height = 0;
	expected2.disk = {0, 0};
	expected2.year = 2007;
	expected2.encoder = "Lavf54.63.100"_c;

	assert_metadata(expected2, *actual2.to_props(), "This Year's Love.m4a");
	assert_equal(true, is_empty(actual2.thumbnail_image), "m4a scan thumbnail");

	const auto loaded = ff_scan_and_load_thumb(ff, load_path2);
	assert_equal(false, loaded.success && is_valid(loaded.thumbnail_surface), "m4a load thumbnail");
}

static void should_preserve_high_audio_sample_rates()
{
	file_scan_result scanned;
	scanned.audio_sample_rate = 192000;
	scanned.audio_channels = 6;

	const auto props = scanned.to_props();

	assert_equal(192000u, props->audio_sample_rate, "192 kHz stays exact in indexed metadata");
	assert_equal(6, static_cast<int>(props->audio_channels), "channel count still converts beside it");
	assert_equal("192kHz", prop::format_audio_sample_rate(props->audio_sample_rate), "high rate formats exactly");
}

// Issue #3 - Cannot remove MP4 tags added in an old version.
// Old versions left tags in the native MP4 'KEYW' atom (surfaced by FFmpeg as the
// "keywords" tag). When a file also carries embedded XMP, dc:subject is the
// authoritative tag list, so a stale container keyword must not resurrect a tag
// that was removed via XMP.
static void should_not_resurrect_container_tags()
{
	metadata_edits unrelated_edits;
	unrelated_edits.title = "unrelated xmp";
	std::string unrelated_xmp;
	metadata_xmp::update(unrelated_xmp, unrelated_edits);

	file_scan_result absent_subject;
	absent_subject.ffmpeg_metadata.emplace_back(str::cache("keywords"), "nativekey");
	absent_subject.metadata.xmp.assign(unrelated_xmp.begin(), unrelated_xmp.end());
	assert_equal("nativekey"_c, absent_subject.to_props()->tags,
	             "unrelated xmp does not suppress native keywords");

	// Serialise an XMP packet whose dc:subject holds only the kept tag - this
	// represents the state right after the stuck tag was removed via XMP.
	metadata_edits edits;
	edits.add_tags = tag_set("keeptag");
	std::string xmp_buffer;
	metadata_xmp::update(xmp_buffer, edits);

	// A scan result carrying the stale container keyword AND the embedded XMP.
	file_scan_result sr;
	sr.ffmpeg_metadata.emplace_back(str::cache("keywords"), "stucktag");
	sr.metadata.xmp.assign(xmp_buffer.begin(), xmp_buffer.end());

	const auto md = sr.to_props();

	// Only the XMP tag survives; the stale KEYW keyword is not merged back in.
	assert_equal("keeptag"_c, md->tags, "xmp authoritative over stale container keyword");

	metadata_edits empty_edits;
	empty_edits.add_tags = tag_set("temporary");
	std::string empty_xmp;
	metadata_xmp::update(empty_xmp, empty_edits);
	metadata_edits clear_edits;
	clear_edits.remove_tags = tag_set("temporary");
	metadata_xmp::update(empty_xmp, clear_edits);

	file_scan_result empty_subject;
	empty_subject.ffmpeg_metadata.emplace_back(str::cache("keywords"), "nativekey");
	empty_subject.metadata.xmp.assign(empty_xmp.begin(), empty_xmp.end());
	assert_equal(true, empty_subject.to_props()->tags.is_empty(),
	             "empty xmp subject suppresses native keywords");
}

static void should_apply_property_level_windows_metadata_precedence()
{
	metadata_edits unrelated_edits;
	unrelated_edits.title = "unrelated xmp";
	std::string unrelated_xmp;
	metadata_xmp::update(unrelated_xmp, unrelated_edits);

	file_scan_result fallback;
	fallback.ffmpeg_metadata.emplace_back(str::cache("WM/Category"), "WindowsTag");
	fallback.ffmpeg_metadata.emplace_back(str::cache("WM/SharedUserRating"), "75");
	fallback.metadata.xmp.assign(unrelated_xmp.begin(), unrelated_xmp.end());
	const auto fallback_metadata = fallback.to_props();
	assert_equal("WindowsTag"_c, fallback_metadata->tags, "unrelated xmp allows windows tags");
	assert_equal(4, fallback_metadata->rating, "unrelated xmp allows windows rating");

	metadata_edits cleared_edits;
	cleared_edits.add_tags = tag_set("temporary");
	cleared_edits.rating = 5;
	std::string cleared_xmp;
	metadata_xmp::update(cleared_xmp, cleared_edits);
	metadata_edits remove_edits;
	remove_edits.remove_tags = tag_set("temporary");
	remove_edits.remove_rating = true;
	metadata_xmp::update(cleared_xmp, remove_edits);

	file_scan_result cleared;
	cleared.ffmpeg_metadata.emplace_back(str::cache("WM/Category"), "StaleTag");
	cleared.ffmpeg_metadata.emplace_back(str::cache("WM/SharedUserRating"), "99");
	cleared.metadata.xmp.assign(cleared_xmp.begin(), cleared_xmp.end());
	const auto cleared_metadata = cleared.to_props();
	assert_equal(true, cleared_metadata->tags.is_empty(), "cleared xmp tags suppress stale windows tags");
	assert_equal(0, cleared_metadata->rating, "cleared xmp rating suppresses stale windows rating");
}

// Matroska stores metadata as EBML SimpleTags, which FFmpeg surfaces verbatim - so the
// upper-case names Matroska specifies (TITLE, RATING, ...) only reach the UI because our
// key matching is case-insensitive. WebM is the same container with a different DocType,
// so it must behave identically. Nothing else in the corpus is EBML.
static void should_scan_matroska(const std::string_view name, const std::string_view video_codec)
{
	files ff;
	const auto actual = ff_scan_file(ff, test_files_folder.combine_file(name));

	prop::item_metadata expected;
	expected.title = "Matroska Test Title"_c;
	expected.artist = "Matroska Test Artist"_c;
	expected.genre = "TestGenre"_c;
	expected.comment = "matroska test comment"_c;
	expected.tags = "alpha beta"_c;
	expected.rating = 4;
	expected.width = 16;
	expected.height = 16;
	expected.duration = 1;
	expected.encoder = "diffractor-test"_c; // Matroska MuxingApp
	expected.video_codec = str::cache(video_codec);

	assert_metadata(expected, *actual.to_props(), name);
}

static void should_scan_mov()
{
	const auto load_path = test_files_folder.combine_file("ipod.mov");

	files ff;
	const auto actual = ff_scan_file(ff, load_path);

	prop::item_metadata expected;
	expected.title = "iPad Help"_c;
	expected.tags = "apple ipad ipod support"_c;
	expected.comment = "What to do if you ipad dies"_c;
	expected.audio_codec = "aac"_c;
	expected.audio_sample_rate = 32000;
	expected.audio_channels = 1;
	expected.pixel_format = "yuv420"_c;
	expected.duration = 86;
	expected.width = 640;
	expected.height = 480;
	expected.rating = 4;
	expected.video_codec = "mpeg4"_c;
	expected.dates.add_utc(prop::date_source::container_created, df::date_t(2005, 10, 17, 22, 54, 32));
	expected.dates.add_utc(prop::date_source::xmp_modify, df::date_t(2005, 10, 17, 22, 56, 39));
	expected.year = 2005;

	assert_metadata(expected, *actual.to_props(), "ipod.mov");

	const auto load_path2 = test_files_folder.combine_file("StPauls.MOV");
	const auto actual2 = ff_scan_file(ff, load_path2);

	prop::item_metadata expected2;
	// Apple's creation date states the offset of the zone it was made in, so it is the time the camera's
	// clock showed there, as a photo's EXIF time with its offset is.
	expected2.dates.add(prop::date_source::container_created, df::date_t(2011, 3, 13, 15, 13, 49), 0);
	expected2.audio_codec = "aac"_c;
	// Issue #61: the QuickTime ISO6709 location tag must reach the coordinate, or the world map
	// cannot plot the video.
	expected2.coordinate = {51.51420, -0.09850};
	// StPauls.MOV carries a Windows Explorer 'London' tag (WM/Category, no XMP),
	// now surfaced by the FFmpeg 'Xtra' reader. (#123)
	expected2.tags = "London"_c;
	expected2.width = 640;
	expected2.height = 480;
	expected2.duration = 10;
	expected2.audio_sample_rate = 44100;
	expected2.audio_channels = 1;
	expected2.video_codec = "h264"_c;
	expected2.camera_manufacturer = "Apple"_c;
	expected2.camera_model = "iPhone 3GS"_c;
	expected2.encoder = "4.3"_c;
	expected2.pixel_format = "yuv420"_c;
	expected2.year = 2011;

	assert_metadata(expected2, *actual2.to_props(), "StPauls.MOV");
}

static void should_scan_avi()
{
	const auto load_path = test_files_folder.combine_file("Byzantium.avi");

	files ff;
	const auto actual = ff_scan_file(ff, load_path);

	prop::item_metadata expected;
	expected.audio_codec = "wmav2"_c;
	expected.title = "Byzantium"_c;
	expected.comment =
		"John Romer recreates the glory and history of Byzantium. From the Hagia Sophia in present-day Istanbul to the looted treasures of the empire now located in St. Marks in Venice."_c;
	expected.audio_sample_rate = 48000;
	expected.audio_channels = 2;
	expected.duration = 12;
	expected.width = 854;
	expected.height = 480;
	expected.video_codec = "wmv3"_c;
	expected.tags = "Byzantium History Turkey"_c;
	expected.pixel_format = "yuv420"_c;

	assert_metadata(expected, *actual.to_props(), "Byzantium.avi");
}

static void should_scan_raw()
{
	const auto load_path = test_files_folder.combine("raw").combine_file("Screws.CR2");

	prop::item_metadata expected;
	expected.file_name = "Screws.CR2"_c;
	expected.copyright_notice = "Copyright"_c;
	expected.tags = "Desk Macro Screws"_c;
	expected.title = "Screws on Desk"_c;
	expected.description = "This is a Description"_c;
	expected.comment = "This is a Comment"_c;
	expected.rating = 4;
	expected.width = 3522;
	expected.height = 2348;
	expected.f_number = 2.8f;
	expected.camera_manufacturer = "Canon"_c;
	expected.camera_model = "EOS-1D Mark II"_c;
	expected.exposure_time = 1 / 25.0f;
	expected.focal_length = 100;
	expected.iso_speed = 100;
	expected.pixel_format = "RGBG"_c;
	expected.dates.add(prop::date_source::embedded_created, df::date_t(2011, 9, 23, 23, 49, 16));

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	const auto actual = metadata_from_cache(index, load_path);
	assert_metadata(expected, *actual, "Screws.CR2");
}

static void should_scan_mod()
{
	constexpr auto file_name = "giana.mod";
	const auto load_path = test_files_folder.combine("retro").combine_file(file_name);

	files ff;
	const auto actual = ff_scan_file(ff, load_path);
	const auto props = actual.to_props();

	assert_equal("giana!"_c, props->title, "title", file_name);
	assert_equal("Generic ProTracker or compatible"_c, props->encoder, "encoder", file_name);
	assert_equal(48000u, props->audio_sample_rate, "audio_sample_rate", file_name);
}

static std::string png_test_xmp(int rating);

static void should_scan_heic()
{
	constexpr auto file_name = "melnik.heic";
	const auto load_path = test_files_folder.combine_file(file_name);

	files ff;
	const auto actual = ff_scan_file(ff, load_path);
	const auto props = actual.to_props();

	assert_equal(4000, props->width, "width", file_name);
	assert_equal(2252, props->height, "height", file_name);
	assert_equal("yuv420", props->pixel_format, "pixel_format", file_name);
}

static void should_not_double_apply_heif_rotation()
{
	// This asset carries an 'irot' transform property (270 degrees CCW) alongside an Exif
	// Orientation tag (6) describing the same quarter turn, which is what phone cameras
	// write. libheif applies 'irot' itself: it swaps the dimensions reported by
	// heif_image_handle_get_width/height and rotates the decoded pixels. Applying the
	// Exif orientation on top of that would rotate the image a second time.
	constexpr auto file_name = "melnik-rotated.heic";
	const auto load_path = test_files_folder.combine("excluded1").combine_file(file_name);

	files ff;
	const auto actual = ff_scan_file(ff, load_path);
	const auto props = actual.to_props();

	// Guard the premise: the file really does carry an Exif block with Orientation 6.
	assert_equal(false, actual.metadata.exif.empty(), "exif present", file_name);

	// The stored image is 4000x2252, so the upright image is 2252x4000.
	assert_equal(2252, props->width, "width", file_name);
	assert_equal(4000, props->height, "height", file_name);

	// Already upright, so no further rotation may be requested.
	assert_equal(static_cast<int>(ui::orientation::top_left), static_cast<int>(props->orientation),
	             "orientation", file_name);

	// The display path decodes the same item, so it must agree with the scan rather than
	// rotating a second time - a mismatch here shows as a thumbnail facing the wrong way.
	const auto loaded = ff.load(load_path, false);
	assert_equal(true, is_valid(loaded.s), "loaded", file_name);
	assert_equal(static_cast<int>(ui::orientation::top_left), static_cast<int>(loaded.orientation()),
	             "loaded orientation", file_name);
	assert_equal(2252, loaded.dimensions().cx, "loaded width", file_name);
	assert_equal(4000, loaded.dimensions().cy, "loaded height", file_name);

	// The stored thumbnail is a separate item that need not carry the same 'irot', so its
	// orientation is resolved from its own properties. The upright image is portrait, so the
	// thumbnail must also be portrait once its own orientation is applied.
	const auto* const ft = files::file_type_from_name(load_path);
	const auto with_thumb = ff.scan_file(load_path, true, ft, {}, {}, scan_intent::index);
	assert_equal(true, ui::is_valid(with_thumb.thumbnail_surface), "thumbnail", file_name);

	const auto thumb_extent = with_thumb.thumbnail_surface->dimensions();
	const auto thumb_upright = ui::flips_xy(with_thumb.thumbnail_surface->orientation())
		                           ? sizei{thumb_extent.cy, thumb_extent.cx}
		                           : thumb_extent;
	assert_equal(true, thumb_upright.cy > thumb_upright.cx, "thumbnail is upright portrait", file_name);
}

static void should_scan_avif()
{
	constexpr auto file_name = "hato.profile0.10bpc.yuv420.avif";
	const auto load_path = test_files_folder.combine_file(file_name);

	files ff;
	const auto actual = ff_scan_file(ff, load_path);
	const auto props = actual.to_props();

	assert_equal(3078, props->width, "width", file_name);
	assert_equal(2048, props->height, "height", file_name);
	// libheif 1.23 reports the file's native chroma (yuv420, per AV1 profile0) via
	// heif_image_handle_get_preferred_decoding_colorspace; 1.18 returned yuv444.
	assert_equal("yuv420", props->pixel_format, "pixel_format", file_name);
}

static ui::surface_ptr make_png_metadata_surface()
{
	auto surface = std::make_shared<ui::surface>();
	surface->alloc(4, 4, ui::texture_format::RGB);

	for (auto y = 0; y < 4; ++y)
	{
		auto* const line = surface->pixels_line(y);
		for (auto x = 0; x < 4; ++x)
		{
			line[x * 4 + 0] = static_cast<uint8_t>(20 + x * 10);
			line[x * 4 + 1] = static_cast<uint8_t>(30 + y * 10);
			line[x * 4 + 2] = 40;
			line[x * 4 + 3] = 0xff;
		}
	}

	return surface;
}

static void append_png_be32(df::blob& out, const uint32_t value)
{
	out.push_back(static_cast<uint8_t>(value >> 24));
	out.push_back(static_cast<uint8_t>(value >> 16));
	out.push_back(static_cast<uint8_t>(value >> 8));
	out.push_back(static_cast<uint8_t>(value));
}

static void insert_png_chunk_before_iend(df::blob& png, const char (&type)[5], const df::cspan data,
                                         const bool valid_crc = true)
{
	df::blob chunk;
	append_png_be32(chunk, static_cast<uint32_t>(data.size));
	const auto type_start = chunk.size();
	chunk.insert(chunk.end(), type, type + 4);
	chunk.insert(chunk.end(), data.data, data.data + data.size);

	auto crc = crc32(0, Z_NULL, 0);
	crc = crc32(crc, chunk.data() + type_start, static_cast<uInt>(4 + data.size));
	append_png_be32(chunk, valid_crc ? crc : crc ^ 0x01020304u);

	assert_equal(true, png.size() >= 12u, "png has an IEND chunk");
	png.insert(png.end() - 12, chunk.begin(), chunk.end());
}

static df::blob make_png_text_chunk_payload(const bool compressed, const std::string_view xmp)
{
	constexpr auto png_xmp_text_key = "XML:com.adobe.xmp";
	df::blob data;
	data.insert(data.end(), png_xmp_text_key, png_xmp_text_key + strlen(png_xmp_text_key) + 1);
	data.push_back(compressed ? 1 : 0);
	data.push_back(0); // zlib
	data.push_back(0); // empty language tag
	data.push_back(0); // empty translated keyword

	if (compressed)
	{
		uLongf compressed_size = compressBound(static_cast<uLong>(xmp.size()));
		const auto old_size = data.size();
		data.resize(old_size + compressed_size);
		assert_equal(Z_OK, compress2(data.data() + old_size, &compressed_size,
		                             reinterpret_cast<const Bytef*>(xmp.data()), static_cast<uLong>(xmp.size()),
		                             Z_BEST_SPEED), "compress xmp");
		data.resize(old_size + compressed_size);
	}
	else
	{
		data.insert(data.end(), xmp.begin(), xmp.end());
	}

	return data;
}

static df::file_path write_png_with_trailing_chunk(const std::string_view type, const df::blob& data)
{
	files ff;
	const auto image = ff.surface_to_image(make_png_metadata_surface(), {}, {}, ui::image_format::PNG);
	const auto& encoded = image->data();
	df::blob png(encoded.data(), encoded.data() + encoded.size());

	assert_equal(4_z, type.size(), "png chunk type is four bytes");
	const char chunk_type[5] = {type[0], type[1], type[2], type[3], 0};
	insert_png_chunk_before_iend(png, chunk_type, data);

	const auto path = _temps.next_path(".png");
	df::blob_save_to_file(png, path);
	return path;
}

static std::string png_test_xmp(const int rating)
{
	return std::format(
		"<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
		"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
		"xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
		"<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" "
		"xmp:Rating=\"{}\"/>"
		"</rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>",
		rating);
}

// SRC-019 - trailing PNG ancillary metadata is valid and must be read without decoding pixels.
static void should_reject_incomplete_png_compressed_metadata()
{
	auto payload = make_png_text_chunk_payload(true, png_test_xmp(5));
	assert_equal(true, payload.size() > 4, "compressed metadata fixture exists");
	payload.resize(payload.size() - 2);
	const auto path = write_png_with_trailing_chunk("iTXt", payload);
	files codecs;
	const auto scanned = ff_scan_file(codecs, path);
	assert_equal(true, scanned.metadata.xmp.empty(), "truncated zlib checksum does not publish partial metadata");
}

static void should_read_png_metadata_after_idat()
{
	const auto before_path = [&]
	{
		files ff;
		metadata_parts metadata;
		const auto xmp = png_test_xmp(3);
		metadata.xmp.assign(xmp.begin(), xmp.end());
		const auto image = ff.surface_to_image(make_png_metadata_surface(), metadata, {}, ui::image_format::PNG);
		const auto path = _temps.next_path(".png");
		df::blob_save_to_file(image->data(), path);
		return path;
	}();

	const auto trailing_xmp = png_test_xmp(3);
	const auto after_path = write_png_with_trailing_chunk("iTXt", make_png_text_chunk_payload(false, trailing_xmp));

	files ff;
	const auto before = ff_scan_file(ff, before_path).to_props();
	const auto after = ff_scan_file(ff, after_path).to_props();
	assert_equal(before->rating, after->rating, "an XMP packet after IDAT reads like one before IDAT");
	assert_equal(3, after->rating, "trailing XMP rating");

	auto prefixed_exif = make_orientation_exif(ui::orientation::right_top);
	prefixed_exif.insert(prefixed_exif.begin(), exif_signature.begin(), exif_signature.end());
	const auto prefixed_exif_path = write_png_with_trailing_chunk("eXIf", prefixed_exif);
	assert_equal(static_cast<int>(ui::orientation::right_top),
	             static_cast<int>(ff_scan_file(ff, prefixed_exif_path).orientation),
	             "Exif-prefixed trailing eXIf orientation reads");

	const auto compressed_path = write_png_with_trailing_chunk("iTXt", make_png_text_chunk_payload(true, png_test_xmp(4)));
	assert_equal(4, ff_scan_file(ff, compressed_path).to_props()->rating, "compressed trailing XMP reads");

	const auto bad_crc_image = ff.surface_to_image(make_png_metadata_surface(), {}, {}, ui::image_format::PNG);
	const auto& bad_crc_encoded = bad_crc_image->data();
	df::blob bad_crc(bad_crc_encoded.data(), bad_crc_encoded.data() + bad_crc_encoded.size());
	const auto bad = make_png_text_chunk_payload(false, png_test_xmp(5));
	insert_png_chunk_before_iend(bad_crc, "iTXt", bad, false);
	const auto bad_crc_path = _temps.next_path(".png");
	df::blob_save_to_file(bad_crc, bad_crc_path);
	assert_equal(0, ff_scan_file(ff, bad_crc_path).to_props()->rating, "bad CRC metadata is ignored");

	const auto truncated_image = ff.surface_to_image(make_png_metadata_surface(), {}, {}, ui::image_format::PNG);
	const auto& truncated_encoded = truncated_image->data();
	df::blob truncated(truncated_encoded.data(), truncated_encoded.data() + truncated_encoded.size());
	const auto payload = make_png_text_chunk_payload(false, png_test_xmp(5));
	append_png_be32(truncated, static_cast<uint32_t>(payload.size() + 100));
	constexpr uint8_t itxt_type[] = {'i', 'T', 'X', 't'};
	truncated.insert(truncated.end(), std::begin(itxt_type), std::end(itxt_type));
	truncated.insert(truncated.end(), payload.begin(), payload.end());
	const auto truncated_path = _temps.next_path(".png");
	df::blob_save_to_file(truncated, truncated_path);
	assert_equal(0, ff_scan_file(ff, truncated_path).to_props()->rating, "truncated trailing metadata is ignored");
}

static void should_scan_webp()
{
	constexpr auto file_name = "lake.webp";
	const auto load_path = test_files_folder.combine_file(file_name);

	files ff;
	const auto actual = ff_scan_file(ff, load_path);
	const auto props = actual.to_props();

	assert_equal(550, props->width, "width", file_name);
	assert_equal(368, props->height, "height", file_name);
	assert_equal("yuv420", props->pixel_format, "pixel_format", file_name);
}

static void should_scan_jxl()
{
	constexpr auto file_name = "zoltan-tasi.jxl";
	const auto load_path = test_files_folder.combine_file(file_name);

	files ff;
	const auto actual = ff_scan_file(ff, load_path);
	const auto props = actual.to_props();

	assert_equal(true, actual.success, "success", file_name);
	assert_equal(true, props->width > 0 && props->height > 0, "dimensions", file_name);

	// The codestream decodes to a surface matching the scanned dimensions.
	const auto loaded = ff.load(load_path, false);
	assert_equal(false, loaded.is_empty(), "jxl load empty", file_name);
	assert_equal(props->width, loaded.dimensions().cx, "jxl load width", file_name);
	assert_equal(props->height, loaded.dimensions().cy, "jxl load height", file_name);
}

static const metadata_block* find_block(const av_media_info& info, const metadata_standard standard)
{
	for (const auto& b : info.metadata)
	{
		if (b.standard == standard) return &b;
	}

	return nullptr;
}

static const metadata_kv* find_row(const metadata_block& block, const std::string_view key)
{
	for (const auto& r : block.values)
	{
		if (r.key == key) return &r;
	}

	return nullptr;
}

static const metadata_kv* find_row(const metadata_kv_list& rows, const std::string_view key)
{
	for (const auto& r : rows)
	{
		if (r.key == key) return &r;
	}

	return nullptr;
}

static std::string row_value_or_empty(const metadata_kv_list& rows, const std::string_view key)
{
	const auto* const row = find_row(rows, key);
	assert_equal(true, row != nullptr, "metadata row exists", key);
	return row ? row->value : std::string{};
}

// The block written for an orientation alone linked an empty IFD1 whose next-IFD offset was cut to
// two of its four bytes, so every reader that walks the chain read past the end of it. The walk here
// is the one a reader makes, and it has to stay inside the block.
static void should_write_a_complete_orientation_exif()
{
	const auto exif = make_orientation_exif(ui::orientation::right_top);
	const auto u16 = [&exif](const size_t at)
	{
		return static_cast<size_t>(exif[at]) | static_cast<size_t>(exif[at + 1]) << 8;
	};
	const auto u32 = [&u16](const size_t at) { return u16(at) | u16(at + 2) << 16; };

	assert_equal(true, exif.size() >= 8 && exif[0] == 'I' && exif[1] == 'I', "a little-endian TIFF header");

	auto ifd = u32(4);
	auto ifds = 0;

	while (ifd != 0 && ifds < 4)
	{
		assert_equal(true, ifd + 2 <= exif.size(), "each IFD's entry count lies inside the block");
		const auto next_at = ifd + 2 + u16(ifd) * 12;
		assert_equal(true, next_at + 4 <= exif.size(), "and so does all of its next-IFD offset");
		ifd = u32(next_at);
		++ifds;
	}

	assert_equal(1, ifds, "the chain is the one IFD that carries the orientation");
	assert_equal(0x0112_z, u16(10), "whose one entry is the orientation tag");
	assert_equal(static_cast<size_t>(ui::orientation::right_top), u16(18), "holding the value asked for");
}

// The verbose pane is a block inspector: each embedded block must describe its own extent and
// present its own shape, so a reader can tell what the file actually contains.
static void should_present_exif_block_by_ifd()
{
	constexpr auto file_name = "Test.jpg";

	files ff;
	const auto sr = ff_scan_file(ff, test_files_folder.combine_file(file_name));
	const auto info = sr.to_info();
	const auto block = find_block(info, metadata_standard::exif);

	assert_equal(true, block != nullptr, "exif block", file_name);
	assert_equal(true, block->bytes > 0, "exif bytes", file_name);
	assert_equal(true, block->parsed, "exif parsed", file_name);

	// Entries are grouped under the IFD they were read from rather than flattened.
	auto containers = 0;
	auto children = 0;
	auto shaped = 0;

	for (const auto& r : block->values)
	{
		if (r.container)
		{
			++containers;
			assert_equal(0, r.depth, "ifd depth", file_name);
		}
		else
		{
			++children;
			if (!r.shape.empty()) ++shaped;
		}
	}

	assert_equal(true, containers > 0, "exif ifd sections", file_name);
	assert_equal(true, children > 0, "exif entries", file_name);
	assert_equal(children, shaped, "exif entry shapes", file_name);

	// The Exif directory carries the capture settings, so it asks to be open whatever its size.
	const auto* exif_ifd = static_cast<const metadata_kv*>(nullptr);

	for (const auto& r : block->values)
	{
		if (r.id == "exif.ifd.2") exif_ifd = &r;
	}

	assert_equal(true, exif_ifd != nullptr, "exif ifd section", file_name);
	assert_equal(true, exif_ifd->open_by_default, "exif ifd open by default", file_name);
}

// The maker note is a vendor directory carried inside Exif. The listing decodes the makes libexif
// decoded, so it is listed as its own section instead of staying the binary blob the Exif entry reports.
static void should_present_maker_note_section()
{
	constexpr auto file_name = "Test.jpg";

	files ff;
	const auto sr = ff_scan_file(ff, test_files_folder.combine_file(file_name));
	const auto info = sr.to_info();
	const auto block = find_block(info, metadata_standard::exif);

	assert_equal(true, block != nullptr, "exif block", file_name);

	size_t section_index = block->values.size();

	for (size_t i = 0; i < block->values.size(); ++i)
	{
		if (block->values[i].id == "exif.mnote") section_index = i;
	}

	assert_equal(true, section_index < block->values.size(), "maker note section", file_name);

	const auto& section = block->values[section_index];

	assert_equal(true, section.container, "maker note is a section", file_name);
	assert_equal(0, section.depth, "maker note section depth", file_name);
	// This is a Canon body, so the section names the make rather than reading as an anonymous blob.
	assert_equal(true, section.key.find("Canon") != std::string::npos, "maker note names the make", file_name);

	// The decoded entries follow it, before any later section.
	auto decoded = 0;

	for (size_t i = section_index + 1; i < block->values.size() && !block->values[i].container; ++i)
	{
		if (!block->values[i].id.starts_with("exif.mnote.")) break;

		++decoded;
		assert_equal(1, block->values[i].depth, "maker note value depth", file_name);
		assert_equal(false, block->values[i].key.empty(), "maker note value name", file_name);
		assert_equal(false, block->values[i].shape.empty(), "maker note value shape", file_name);
	}

	assert_equal(true, decoded > 0, "maker note values", file_name);

	// The raw Exif entry is kept alongside the decoded reading rather than replaced by it.
	auto has_raw_entry = false;

	for (const auto& r : block->values)
	{
		if (r.id == "exif.2.927c") has_raw_entry = true;
	}

	assert_equal(true, has_raw_entry, "maker note raw entry retained", file_name);
}

// Derived rows are arithmetic over tags listed above them. Test.jpg is an 18 MP EOS 7D shot at f/6.3
// and 1/100, so both figures are known independently of the code that computes them.
static void should_present_derived_exif_section()
{
	constexpr auto file_name = "Test.jpg";

	files ff;
	const auto sr = ff_scan_file(ff, test_files_folder.combine_file(file_name));
	const auto info = sr.to_info();
	const auto block = find_block(info, metadata_standard::exif);

	assert_equal(true, block != nullptr, "exif block", file_name);

	const auto* section = static_cast<const metadata_kv*>(nullptr);

	for (const auto& r : block->values)
	{
		if (r.id == "exif.derived") section = &r;
	}

	assert_equal(true, section != nullptr, "derived section", file_name);
	assert_equal(true, section->container, "derived is a section", file_name);
	assert_equal(0, section->depth, "derived section depth", file_name);

	const auto find_derived = [&block](const std::string_view key) -> const metadata_kv*
	{
		for (const auto& r : block->values)
		{
			if (!r.container && r.id.starts_with("exif.derived.") && r.key == key) return &r;
		}

		return nullptr;
	};

	// 2 * log2(6.3) - log2(1/100) = 11.96
	const auto* const light = find_derived("Light value");
	assert_equal(true, light != nullptr, "light value row", file_name);
	assert_equal("12.0 EV", light->value, "light value", file_name);

	// 5184 x 3456
	const auto* const megapixels = find_derived("Megapixels");
	assert_equal(true, megapixels != nullptr, "megapixels row", file_name);
	assert_equal("17.9 MP", megapixels->value, "megapixels", file_name);

	// A derived row states what it was computed from, so it cannot be mistaken for a recorded tag.
	assert_equal(false, light->shape.empty(), "light value shape", file_name);
	assert_equal(1, light->depth, "light value depth", file_name);

	// This body records no 35 mm equivalent, so nothing that needs one is invented.
	assert_equal(true, find_derived("Crop factor") == nullptr, "no crop factor without focal.35", file_name);
	assert_equal(true, find_derived("Hyperfocal distance") == nullptr, "no hyperfocal without focal.35",
	             file_name);
}

// A lossless rotate corrects the EXIF block's orientation and dimensions. When it rewrote the block
// through libexif, whose loading defaults drop every tag it has no name for - maker notes above all -
// a rotate used to take the camera's own record of the shot with it, silently and with no way back.
static void should_keep_unknown_exif_tags_through_a_rotate()
{
	std::vector<uint8_t> buf;
	const auto put16 = [&buf](const uint16_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
	};
	const auto put32 = [&buf](const uint32_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
		buf.push_back(static_cast<uint8_t>(v >> 16));
		buf.push_back(static_cast<uint8_t>(v >> 24));
	};
	const auto put_entry = [&put16, &put32](const uint16_t tag, const uint16_t fmt, const uint32_t count,
	                                        const uint32_t value)
	{
		put16(tag);
		put16(fmt);
		put32(count);
		put32(value);
	};

	constexpr uint16_t fmt_short = 3;
	constexpr uint16_t private_tag = 0xfde8;

	// "Exif\0\0" ahead of the TIFF header, which is what fix_dims is handed.
	for (const auto c : std::string_view("Exif\0\0", 6)) buf.push_back(static_cast<uint8_t>(c));

	put16(0x4949); // little-endian TIFF header
	put16(0x002a);
	put32(8);

	put16(2);
	put_entry(0x0112, fmt_short, 1, 6); // Orientation, rotated
	put_entry(private_tag, fmt_short, 1, 0x1234); // a tag the listing has no name for
	put32(0);

	const auto contains_private_tag = [](const df::blob& block)
	{
		for (const auto& kv : metadata_exif::to_info(df::cspan{block.data(), block.size()}))
		{
			if (kv.key.find("FDE8") != std::string::npos || kv.id.find("fde8") != std::string::npos) return true;
		}

		return false;
	};

	const df::blob before(buf.begin(), buf.end());
	assert_equal(true, contains_private_tag(before), "the fixture carries a tag the listing does not name");

	const auto after = metadata_exif::fix_dims(df::span{buf.data(), buf.size()}, 640, 480);
	assert_equal(false, after.empty(), "the rotate rewrote the block");
	assert_equal(true, contains_private_tag(after), "and the unnamed tag survived it");
}

// The focal-length family needs a 35 mm equivalent, which the EOS 7D fixture does not record. A
// synthetic block fixes 15 mm at 24 mm equivalent and f/6.3 so each figure has one known answer.
static void should_derive_focal_length_facts()
{
	std::vector<uint8_t> buf;
	const auto put16 = [&buf](const uint16_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
	};
	const auto put32 = [&buf](const uint32_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
		buf.push_back(static_cast<uint8_t>(v >> 16));
		buf.push_back(static_cast<uint8_t>(v >> 24));
	};
	const auto put_entry = [&put16, &put32](const uint16_t tag, const uint16_t fmt, const uint32_t count,
	                                        const uint32_t value)
	{
		put16(tag);
		put16(fmt);
		put32(count);
		put32(value);
	};

	constexpr uint16_t fmt_short = 3;
	constexpr uint16_t fmt_long = 4;
	constexpr uint16_t fmt_rational = 5;

	constexpr uint32_t ifd0_offset = 8;
	constexpr uint32_t exif_ifd_offset = ifd0_offset + 2 + 12 + 4; // one IFD0 entry: the Exif pointer
	constexpr uint32_t data_offset = exif_ifd_offset + 2 + 3 * 12 + 4; // three Exif entries

	put16(0x4949); // little-endian TIFF header
	put16(0x002a);
	put32(ifd0_offset);

	put16(1);
	put_entry(0x8769, fmt_long, 1, exif_ifd_offset); // ExifIFDPointer
	put32(0);

	put16(3);
	put_entry(0x920a, fmt_rational, 1, data_offset); // FocalLength, 15 mm
	put_entry(0xa405, fmt_short, 1, 24); // FocalLengthIn35mmFilm
	put_entry(0x829d, fmt_rational, 1, data_offset + 8); // FNumber, f/6.3
	put32(0);

	put32(15);
	put32(1);
	put32(63);
	put32(10);

	const auto kv = metadata_exif::to_info(df::cspan{buf.data(), buf.size()});

	const auto find_derived = [&kv](const std::string_view key) -> const metadata_kv*
	{
		for (const auto& r : kv)
		{
			if (!r.container && r.id.starts_with("exif.derived.") && r.key == key) return &r;
		}

		return nullptr;
	};

	// 24 / 15
	const auto* const crop = find_derived("Crop factor");
	assert_equal(true, crop != nullptr, "crop factor row");
	assert_equal("1.60x", crop->value, "crop factor");

	// 2 * atan(36 / 48) = 73.74 degrees
	const auto* const fov = find_derived("Field of view");
	assert_equal(true, fov != nullptr, "field of view row");
	assert_equal("73.7\u00b0 horizontal", fov->value, "field of view");

	// 0.03 / 1.6
	const auto* const coc = find_derived("Circle of confusion");
	assert_equal(true, coc != nullptr, "circle of confusion row");
	assert_equal("0.019 mm", coc->value, "circle of confusion");

	// 15^2 / (6.3 * 0.01875) + 15 = 1919.8 mm
	const auto* const hyperfocal = find_derived("Hyperfocal distance");
	assert_equal(true, hyperfocal != nullptr, "hyperfocal row");
	assert_equal("1.92 m", hyperfocal->value, "hyperfocal distance");
}

// The block inspector reports what the file holds: an entry the listing has no name for is numbered
// rather than dropped, and a mandatory entry the file omits is not invented to fill the gap.
static void should_present_exif_tags_as_stored()
{
	// DNG UniqueCameraModel: outside the listing's tag table, libexif's, so it has no name anywhere.
	constexpr uint16_t unnamed_tag = 0xc614;
	constexpr uint16_t fmt_ascii = 2;
	const std::string_view value = "Diffractor";

	std::vector<uint8_t> buf;
	const auto put16 = [&buf](const uint16_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
	};
	const auto put32 = [&buf](const uint32_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
		buf.push_back(static_cast<uint8_t>(v >> 16));
		buf.push_back(static_cast<uint8_t>(v >> 24));
	};

	constexpr uint32_t ifd0_offset = 8;
	constexpr uint32_t data_start = ifd0_offset + 2 + 12 + 4; // one entry
	const auto size = static_cast<uint32_t>(value.size() + 1);

	put16(0x4949); // little-endian TIFF header
	put16(0x002a);
	put32(ifd0_offset);
	put16(1); // entry count
	put16(unnamed_tag);
	put16(fmt_ascii);
	put32(size);
	put32(data_start);
	put32(0); // no IFD1
	buf.insert(buf.end(), value.begin(), value.end());
	buf.push_back(0);

	const auto kv = metadata_exif::to_info(df::cspan{buf.data(), buf.size()});

	const auto* row = static_cast<const metadata_kv*>(nullptr);

	for (const auto& r : kv)
	{
		if (r.id == "exif.0.c614") row = &r;
	}

	assert_equal(true, row != nullptr, "unnamed tag row");
	assert_equal("Tag 0xc614", row->key, "unnamed tag key");
	assert_equal("Diffractor", row->value, "unnamed tag value");
	assert_equal(1, row->depth, "unnamed tag depth");

	// The file holds exactly one entry, so the directory reports one and nothing was added to it.
	auto entries = 0;

	for (const auto& r : kv)
	{
		if (!r.container && r.id.starts_with("exif.0.")) ++entries;
	}

	assert_equal(1, entries, "entries in IFD0");

	// ColorSpace is mandatory in the Exif directory, and a reader that fixes blocks to the specification
	// would invent it.
	for (const auto& r : kv)
	{
		assert_equal(false, r.key == "ColorSpace", "no fabricated mandatory tag");
	}
}

// An embedded thumbnail is a picture, so the pane shows it rather than dumping its bytes, and shows
// it without being asked. A payload that will not decode keeps the hex dump, which stays closed.
static void should_present_embedded_thumbnail_as_an_image()
{
	constexpr auto file_name = "Test.jpg";

	files ff;
	const auto sr = ff_scan_file(ff, test_files_folder.combine_file(file_name));
	const auto info = sr.to_info();
	const auto block = find_block(info, metadata_standard::exif);

	assert_equal(true, block != nullptr, "exif block", file_name);

	const auto* thumbnail = static_cast<const metadata_kv*>(nullptr);

	for (const auto& r : block->values)
	{
		if (r.id == "exif.thumbnail") thumbnail = &r;
	}

	assert_equal(true, thumbnail != nullptr, "embedded thumbnail row", file_name);

	const auto* const image = std::get_if<metadata_image_detail>(&thumbnail->detail);

	assert_equal(true, image != nullptr, "thumbnail is an image", file_name);
	assert_equal(true, ui::is_valid(image->surface), "thumbnail surface", file_name);
	assert_equal(true, thumbnail->open_by_default, "thumbnail opens by default", file_name);

	// The surface is the payload's own size. Handing the decoder a target extent enlarges a small
	// payload to fill it, which draws blurred at a size the file never held.
	assert_equal(160, static_cast<int>(image->surface->width()), "thumbnail width", file_name);
	assert_equal(120, static_cast<int>(image->surface->height()), "thumbnail height", file_name);

	// The bytes are not also kept as a dump: one reading of the payload, not two.
	assert_equal(true, std::get_if<metadata_binary_detail>(&thumbnail->detail) == nullptr,
	             "thumbnail is not a hex dump", file_name);

	// Payloads that are not images keep their dump, and a dump is never opened unasked.
	auto binary_rows = 0;

	for (const auto& r : block->values)
	{
		const auto* const binary = std::get_if<metadata_binary_detail>(&r.detail);
		if (!binary) continue;

		++binary_rows;
		assert_equal(false, binary->is_image, "undecoded payload is not marked an image", file_name);
		assert_equal(false, r.open_by_default, "hex dump stays closed", file_name);
		assert_equal(true, binary->bytes.size() <= str::max_hex_dump_bytes, "hex dump is bounded", file_name);
	}

	// The maker note and other binary entries are still reported, so the check above means something.
	assert_equal(true, binary_rows > 0, "binary rows", file_name);
}

// A raw file carries the same kind of detail as Exif, so LibRaw's flat report is grouped into
// sections and any xmp sidecar is presented as the file's xmp block.
static void should_present_raw_block_sections()
{
	constexpr auto file_name = "Gherkin.CR2";

	files ff;
	const auto sr = ff_scan_file(ff, test_files_folder.combine_file(file_name), "Gherkin.xmp");
	const auto info = sr.to_info();
	const auto block = find_block(info, metadata_standard::raw);

	assert_equal(true, block != nullptr, "raw block", file_name);

	auto containers = 0;
	auto children = 0;
	auto orphans = 0;

	for (const auto& r : block->values)
	{
		if (r.container)
		{
			++containers;
			assert_equal(0, r.depth, "raw section depth", file_name);
			assert_equal(false, r.id.empty(), "raw section id", file_name);
		}
		else
		{
			++children;
			if (r.depth != 1) ++orphans;
		}
	}

	assert_equal(true, containers > 1, "raw sections", file_name);
	assert_equal(true, children > 0, "raw values", file_name);
	assert_equal(0, orphans, "raw value depth", file_name);

	// No section is left as an empty heading.
	for (size_t i = 0; i < block->values.size(); ++i)
	{
		if (block->values[i].container)
		{
			const auto has_child = (i + 1) < block->values.size() && !block->values[i + 1].container;
			assert_equal(true, has_child, "raw section has values", file_name);
		}
	}

	const auto xmp = find_block(info, metadata_standard::xmp);

	assert_equal(true, xmp != nullptr, "raw xmp sidecar block", file_name);
	assert_equal(true, xmp->bytes > 0, "raw xmp sidecar bytes", file_name);
	assert_equal(false, xmp->values.empty(), "raw xmp sidecar values", file_name);
}

static const metadata_kv* find_row_prefix(const metadata_block& block, const std::string_view prefix)
{
	for (const auto& r : block.values)
	{
		if (r.key.starts_with(prefix)) return &r;
	}

	return nullptr;
}

// The structure block reports how the file is built rather than what it says about itself, so it
// survives metadata stripping and shows the traces the encoder left.
static void should_present_jpeg_structure_block()
{
	constexpr auto file_name = "Test.jpg";

	files ff;
	const auto sr = ff_scan_file(ff, test_files_folder.combine_file(file_name));
	const auto info = sr.to_info();
	const auto block = find_block(info, metadata_standard::structure);

	assert_equal(true, block != nullptr, "structure block", file_name);

	const auto summary = find_row_prefix(*block, "Summary");
	const auto segments = find_row_prefix(*block, "Segments");
	const auto quant = find_row_prefix(*block, "Quantisation tables");
	const auto huffman = find_row_prefix(*block, "Huffman tables (");

	assert_equal(true, summary != nullptr, "structure summary", file_name);
	assert_equal(true, summary->open_by_default, "structure summary open", file_name);
	assert_equal(true, segments != nullptr, "structure segments", file_name);
	assert_equal(true, quant != nullptr, "structure quantisation tables", file_name);
	assert_equal(true, huffman != nullptr, "structure huffman tables", file_name);

	// The frame is described, and the trailer is accounted for either way.
	assert_equal(true, find_row(*block, "Encoding") != nullptr, "structure encoding", file_name);
	assert_equal(true, find_row(*block, "Chroma subsampling") != nullptr, "structure subsampling", file_name);
	assert_equal(true, find_row(*block, "End of image marker") != nullptr, "structure eoi", file_name);

	// Quantisation tables carry the coefficient grid rather than being summarised away.
	auto tables_with_detail = 0;

	for (const auto& r : block->values)
	{
		const auto detail = std::get_if<metadata_numeric_detail>(&r.detail);
		if (r.key.starts_with("Table ") && detail && detail->values.size() == 64 && detail->columns == 8)
		{
			++tables_with_detail;
		}
	}

	assert_equal(true, tables_with_detail > 0, "structure table detail", file_name);

	// This file indexes no further images, so no empty heading is offered for them.
	assert_equal(true, find_row_prefix(*block, "Embedded images") == nullptr, "no empty embedded section",
	             file_name);
}

// A Multi-Picture APP2 segment indexes further whole JPEGs inside the same file. Sony.JPG carries
// one, so the images it declares are listed with the extent each claims rather than left inside an
// anonymous APP2 row.
static void should_present_jpeg_embedded_images()
{
	constexpr auto file_name = "Sony.JPG";

	files ff;
	const auto sr = ff_scan_file(ff, test_files_folder.combine_file(file_name));
	const auto info = sr.to_info();
	const auto block = find_block(info, metadata_standard::structure);

	assert_equal(true, block != nullptr, "structure block", file_name);

	const auto section = find_row_prefix(*block, "Embedded images");

	assert_equal(true, section != nullptr, "embedded images section", file_name);
	assert_equal(true, section->container, "embedded images is a section", file_name);

	auto images = 0;

	for (const auto& r : block->values)
	{
		if (!r.id.starts_with("jpeg.mpf.")) continue;

		++images;
		assert_equal(1, r.depth, "embedded image depth", file_name);
		assert_equal(false, r.value.empty(), "embedded image extent", file_name);
	}

	// The primary image plus at least one further stored image is what makes the segment worth writing.
	assert_equal(true, images > 1, "embedded image count", file_name);

	// The first entry is the file itself, which stores a zero offset.
	const auto first = find_row(*block, "Image 1");

	assert_equal(true, first != nullptr, "first embedded image", file_name);
	assert_equal(true, first->value.find("this file") != std::string::npos, "first image is the file",
	             file_name);
}

static void should_present_webp_structure_block()
{
	constexpr auto file_name = "lake.webp";

	files ff;
	const auto sr = ff_scan_file(ff, test_files_folder.combine_file(file_name));
	const auto info = sr.to_info();
	const auto block = find_block(info, metadata_standard::structure);

	assert_equal(true, block != nullptr, "structure block", file_name);
	assert_equal(true, find_row_prefix(*block, "Chunks") != nullptr, "webp chunks", file_name);
	assert_equal(true, find_row(*block, "Compression") != nullptr, "webp compression", file_name);
}

static void should_present_heif_structure_block()
{
	constexpr auto file_name = "melnik.heic";

	files ff;
	const auto sr = ff_scan_file(ff, test_files_folder.combine_file(file_name));
	const auto info = sr.to_info();
	const auto block = find_block(info, metadata_standard::structure);

	assert_equal(true, block != nullptr, "structure block", file_name);
	assert_equal(true, find_row(*block, "Major brand") != nullptr, "heif brand", file_name);
	assert_equal(true, find_row(*block, "Primary item id") != nullptr, "heif primary item", file_name);
	assert_equal(true, find_row(*block, "Stored size") != nullptr, "heif stored size", file_name);
}

static void should_present_icc_block_sections()
{
	constexpr auto file_name = "cmyk.jpg";

	files ff;
	const auto sr = ff_scan_file(ff, test_files_folder.combine_file(file_name));
	const auto info = sr.to_info();
	const auto block = find_block(info, metadata_standard::icc);

	assert_equal(true, block != nullptr, "icc block", file_name);
	assert_equal(true, block->bytes > 0, "icc bytes", file_name);

	// Summary, header and tag directory are all present and deliberately labelled, so decoded
	// values never replace the raw tag list they were derived from.
	const auto summary = find_row(*block, "Summary");
	const auto header = find_row(*block, "Header");

	assert_equal(true, summary != nullptr, "icc summary", file_name);
	assert_equal(true, header != nullptr, "icc header", file_name);
	assert_equal(true, summary->container, "icc summary container", file_name);
	assert_equal(true, header->container, "icc header container", file_name);

	auto tags = 0;

	for (const auto& r : block->values)
	{
		if (r.id.starts_with("icc.tag.")) ++tags;
	}

	assert_equal(true, tags > 0, "icc tag rows", file_name);
}

// A tag names a region of the profile, and several tags may name the same one: the sRGB profile most
// cameras embed points all three tone curves at a single curve. A budget charging every tag for its
// region refused two of the three as though the profile claimed more bytes than it holds. Regions
// that overlap without being shared are still charged in full, which is what stops a crafted
// directory copying many times the profile's own size.
static void should_read_icc_tags_that_share_a_region()
{
	const auto put32 = [](std::vector<uint8_t>& out, const size_t at, const uint32_t v)
	{
		out[at] = static_cast<uint8_t>(v >> 24);
		out[at + 1] = static_cast<uint8_t>(v >> 16);
		out[at + 2] = static_cast<uint8_t>(v >> 8);
		out[at + 3] = static_cast<uint8_t>(v);
	};

	// The loader checks the declared size and the 'acsp' signature in the 128 byte header, then
	// reads a count and twelve-byte directory entries of signature, offset and size.
	const auto make_profile = [&put32](const size_t tag_count, const size_t payload_bytes)
	{
		std::vector<uint8_t> profile(128 + 4 + tag_count * 12 + payload_bytes, 0);
		put32(profile, 0, static_cast<uint32_t>(profile.size()));
		put32(profile, 36, 0x61637370u); // acsp
		put32(profile, 128, static_cast<uint32_t>(tag_count));
		return profile;
	};

	const auto rows = [](const std::vector<uint8_t>& profile, const std::string_view prefix)
	{
		auto count = 0;

		for (const auto& row : metadata_icc::to_info({profile.data(), profile.size()}))
		{
			if (row.id.starts_with(prefix)) ++count;
		}

		return count;
	};

	{
		constexpr size_t curve_bytes = 12 + 1000 * 2;
		auto profile = make_profile(3, curve_bytes);
		constexpr auto curve_at = static_cast<uint32_t>(128 + 4 + 3 * 12);
		put32(profile, curve_at, 0x63757276u); // curv
		put32(profile, curve_at + 8, 1000);

		constexpr uint32_t curves[] = {0x72545243u, 0x67545243u, 0x62545243u}; // rTRC gTRC bTRC

		for (size_t i = 0; i < 3; ++i)
		{
			const auto entry = 132 + i * 12;
			put32(profile, entry, curves[i]);
			put32(profile, entry + 4, curve_at);
			put32(profile, entry + 8, static_cast<uint32_t>(curve_bytes));
		}

		assert_equal(3, rows(profile, "icc.tag."), "every tone curve naming the shared curve is read");
		assert_equal(0, rows(profile, "icc.unread"), "and none of them is refused");
	}

	{
		constexpr size_t tag_count = 100;
		constexpr size_t payload_bytes = 4000;
		auto profile = make_profile(tag_count, payload_bytes);
		constexpr auto payload_at = 128 + 4 + tag_count * 12;
		constexpr auto region_bytes = static_cast<uint32_t>(payload_bytes - (tag_count - 1));

		for (size_t i = 0; i < tag_count; ++i)
		{
			const auto entry = 132 + i * 12;
			put32(profile, entry, 0x74000000u + static_cast<uint32_t>(i));
			put32(profile, entry + 4, static_cast<uint32_t>(payload_at + i));
			put32(profile, entry + 8, region_bytes);
		}

		const auto read = rows(profile, "icc.tag.");
		assert_equal(true, read >= 1, "a region that fits is read");
		assert_equal(true, static_cast<size_t>(read) * region_bytes <= profile.size(),
		             "overlapping regions are charged in full, up to the profile's own size");
	}
}

static std::vector<uint8_t> make_icc_profile_with_tags(
	const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& tags)
{
	const auto put32 = [](std::vector<uint8_t>& out, const size_t at, const uint32_t v)
	{
		out[at] = static_cast<uint8_t>(v >> 24);
		out[at + 1] = static_cast<uint8_t>(v >> 16);
		out[at + 2] = static_cast<uint8_t>(v >> 8);
		out[at + 3] = static_cast<uint8_t>(v);
	};

	std::vector<uint8_t> profile(128 + 4 + tags.size() * 12, 0);
	put32(profile, 36, 0x61637370u); // acsp
	put32(profile, 128, static_cast<uint32_t>(tags.size()));

	for (size_t i = 0; i < tags.size(); ++i)
	{
		const auto offset = static_cast<uint32_t>(profile.size());
		profile.insert(profile.end(), tags[i].second.begin(), tags[i].second.end());

		const auto entry = 132 + i * 12;
		put32(profile, entry, tags[i].first);
		put32(profile, entry + 4, offset);
		put32(profile, entry + 8, static_cast<uint32_t>(tags[i].second.size()));
	}

	put32(profile, 0, static_cast<uint32_t>(profile.size()));
	return profile;
}

static std::vector<uint8_t> make_icc_desc_tag(const std::string_view text)
{
	std::vector<uint8_t> tag;
	const auto append32 = [&tag](const uint32_t v)
	{
		tag.push_back(static_cast<uint8_t>(v >> 24));
		tag.push_back(static_cast<uint8_t>(v >> 16));
		tag.push_back(static_cast<uint8_t>(v >> 8));
		tag.push_back(static_cast<uint8_t>(v));
	};

	append32(0x64657363u); // desc
	append32(0);
	append32(static_cast<uint32_t>(text.size() + 1));
	tag.insert(tag.end(), text.begin(), text.end());
	tag.push_back(0);
	return tag;
}

static std::vector<uint8_t> make_icc_text_tag(const std::string_view text)
{
	std::vector<uint8_t> tag;
	const auto append32 = [&tag](const uint32_t v)
	{
		tag.push_back(static_cast<uint8_t>(v >> 24));
		tag.push_back(static_cast<uint8_t>(v >> 16));
		tag.push_back(static_cast<uint8_t>(v >> 8));
		tag.push_back(static_cast<uint8_t>(v));
	};

	append32(0x74657874u); // text
	append32(0);
	tag.insert(tag.end(), text.begin(), text.end());
	tag.push_back(0);
	return tag;
}

static std::vector<uint8_t> make_icc_mluc_tag(const std::vector<std::string_view>& texts)
{
	std::vector<uint8_t> tag;
	const auto append16 = [&tag](const uint16_t v)
	{
		tag.push_back(static_cast<uint8_t>(v >> 8));
		tag.push_back(static_cast<uint8_t>(v));
	};
	const auto append32 = [&tag](const uint32_t v)
	{
		tag.push_back(static_cast<uint8_t>(v >> 24));
		tag.push_back(static_cast<uint8_t>(v >> 16));
		tag.push_back(static_cast<uint8_t>(v >> 8));
		tag.push_back(static_cast<uint8_t>(v));
	};

	append32(0x6d6c7563u); // mluc
	append32(0);
	append32(static_cast<uint32_t>(texts.size()));
	append32(12);

	std::vector<std::vector<uint8_t>> encoded;
	for (const auto text : texts)
	{
		std::vector<uint8_t> bytes;
		for (const auto c : text)
		{
			bytes.push_back(0);
			bytes.push_back(static_cast<uint8_t>(c));
		}
		encoded.emplace_back(std::move(bytes));
	}

	auto offset = static_cast<uint32_t>(4 + 12 + texts.size() * 12);
	for (size_t i = 0; i < texts.size(); ++i)
	{
		append16(0x656eu); // en
		append16(static_cast<uint16_t>(0x5553u + i)); // US, then another country code
		append32(static_cast<uint32_t>(encoded[i].size()));
		append32(offset);
		offset += static_cast<uint32_t>(encoded[i].size());
	}

	for (const auto& bytes : encoded)
	{
		tag.insert(tag.end(), bytes.begin(), bytes.end());
	}

	return tag;
}

static void should_decode_icc_text_exactly()
{
	const std::string long_profile(512, 'P');
	const std::string long_copyright(512, 'C');
	const auto profile = make_icc_profile_with_tags({
		{0x64657363u, make_icc_desc_tag(long_profile)},
		{0x63707274u, make_icc_text_tag(long_copyright)}
	});

	for (auto i = 0; i < 8; ++i)
	{
		std::vector<std::string> churn(128, std::string(1024, static_cast<char>('a' + i)));
		const auto rows = metadata_icc::to_info({profile.data(), profile.size()});

		assert_equal(long_profile, row_value_or_empty(rows, "Profile"), "profile description");
		assert_equal(long_copyright, row_value_or_empty(rows, "Copyright"), "copyright text");
		assert_equal(long_profile, row_value_or_empty(rows, "desc"), "description tag text");
		assert_equal(long_copyright, row_value_or_empty(rows, "cprt"), "copyright tag text");
	}
}

static void should_decode_icc_mluc_offsets()
{
	{
		const auto profile = make_icc_profile_with_tags({{0x64657363u, make_icc_mluc_tag({"AB"})}});
		const auto rows = metadata_icc::to_info({profile.data(), profile.size()});
		assert_equal("AB", row_value_or_empty(rows, "Profile"), "short mluc");
		assert_equal("AB", row_value_or_empty(rows, "desc"), "short mluc tag");
	}

	{
		const std::string long_text(300, 'L');
		const auto profile = make_icc_profile_with_tags({{0x64657363u, make_icc_mluc_tag({long_text})}});
		const auto rows = metadata_icc::to_info({profile.data(), profile.size()});
		assert_equal(long_text, row_value_or_empty(rows, "Profile"), "long mluc");
	}

	{
		const auto profile = make_icc_profile_with_tags({{0x64657363u, make_icc_mluc_tag({"First", "Second"})}});
		const auto rows = metadata_icc::to_info({profile.data(), profile.size()});
		assert_equal("First", row_value_or_empty(rows, "Profile"), "first valid mluc record");
	}

	{
		auto tag = make_icc_mluc_tag({"Odd"});
		tag[4 + 16] = 0;
		tag[4 + 17] = 0;
		tag[4 + 18] = 0;
		tag[4 + 19] = 5;
		const auto profile = make_icc_profile_with_tags({{0x64657363u, tag}});
		const auto rows = metadata_icc::to_info({profile.data(), profile.size()});
		assert_equal("binary, 34 bytes", row_value_or_empty(rows, "desc"), "odd mluc string is rejected");
	}

	{
		auto tag = make_icc_mluc_tag({"Bad"});
		tag[4 + 20] = 0xff;
		tag[4 + 21] = 0xff;
		tag[4 + 22] = 0xff;
		tag[4 + 23] = 0xff;
		const auto profile = make_icc_profile_with_tags({{0x64657363u, tag}});
		const auto rows = metadata_icc::to_info({profile.data(), profile.size()});
		assert_equal("binary, 34 bytes", row_value_or_empty(rows, "desc"), "out-of-range mluc offset is rejected");
	}
}

// The description panel presents one section for every prose field, so the field list drives its
// header name, its ordering, and which entries collapse as repeats.
static void should_collect_descriptive_fields()
{
	prop::item_metadata none;
	assert_equal(size_t{0}, prop::descriptive_fields(none).size(), "no prose fields");

	prop::item_metadata one;
	one.comment = "A note"_c;
	const auto only_comment = prop::descriptive_fields(one);
	assert_equal(size_t{1}, only_comment.size(), "single prose field");
	assert_equal("comment", only_comment[0].id, "lone field keeps its own identity");
	assert_equal(false, only_comment[0].duplicate, "a lone field is never a repeat");

	prop::item_metadata all;
	all.comment = "Same text"_c;
	all.description = "Same text"_c;
	all.synopsis = "Different text"_c;
	const auto ordered = prop::descriptive_fields(all);
	assert_equal(size_t{3}, ordered.size(), "every populated field listed");
	assert_equal("description", ordered[0].id, "description leads");
	assert_equal("synopsis", ordered[1].id, "synopsis follows");
	assert_equal("comment", ordered[2].id, "comment last");
	assert_equal(false, ordered[0].duplicate, "leading field is the original");
	assert_equal(false, ordered[1].duplicate, "distinct text is not a repeat");
	assert_equal(true, ordered[2].duplicate, "text already shown is marked a repeat");
}

static void should_scan_info_from_title()
{
	const auto assert_name = [](const std::string_view name, const media_name_props& expected)
	{
		const auto actual = scan_info_from_title(name);
		assert_equal(expected.show, actual.show, "show", name);
		assert_equal(expected.title, actual.title, "title", name);
		assert_equal(expected.season, actual.season, "season", name);
		assert_equal(expected.episode, actual.episode, "episode", name);
		assert_equal(expected.episode_of, actual.episode_of, "episode_of", name);
		assert_equal(expected.year, actual.year, "year", name);
	};

	assert_name("Game.of.Thrones.S02E06.HDTV.x264 - 2HD.mp4",
	            {.show = "Game of Thrones", .season = 2, .episode = 6});
	assert_name("It's.a.Wonderful.Life.1946.720p.BluRay.x264.YIFY.mp4",
	            {.title = "It's a Wonderful Life", .year = 1946});
	assert_name("The.Show.S01E02.The.Beginning.2160p.WEB-DL.HEVC.HDR",
	            {.show = "The Show", .title = "The Beginning", .season = 1, .episode = 2});
	assert_name("The.Show.S01E02.The.Beginning.DV.HEVC",
	            {.show = "The Show", .title = "The Beginning", .season = 1, .episode = 2});
	assert_name("Show_Name_S02E06_1080p", {.show = "Show Name", .season = 2, .episode = 6});
	assert_name("Show.Name.1x02.720p", {.show = "Show Name", .season = 1, .episode = 2});
	assert_name("Documentary.Part.02of10.1080p", {.show = "Documentary Part", .episode = 2, .episode_of = 10});
	assert_name("Holiday.Video.1920x1080", {.title = "Holiday Video"});
	assert_name("Movie.Title.2024", {.title = "Movie Title", .year = 2024});
	assert_name("Movie.Title.(2024).2160p", {.title = "Movie Title", .year = 2024});
	assert_name("The.Show.(2020).S01E02.1080p",
	            {.show = "The Show", .season = 1, .episode = 2, .year = 2020});
	assert_name("The.Show.S01E02.[GROUP].Episode.Title.1080p",
	            {.show = "The Show", .title = "Episode Title", .season = 1, .episode = 2});
	assert_name("Show.S999999999999E1.1080p", {.title = "Show S999999999999E1"});
	assert_name("Series.10of02.1080p", {.title = "Series 10of02"});
	assert_name("Family.Holiday.Video", {});
}

static void should_parse_exif_tags()
{
	// Builds a minimal little-endian TIFF/EXIF block that exercises the tags added to
	// the EXIF parser: Software (-> encoder), Artist (-> artist), the Windows XP string
	// tags (XPTitle/XPAuthor/XPKeywords/XPSubject) and the APEX MaxApertureValue /
	// ShutterSpeedValue fallbacks. XP values use ASCII payloads so decoding is
	// deterministic; the point of this test is the tag-dispatch and fallback logic.
	struct exif_entry
	{
		uint16_t tag;
		uint16_t fmt;
		uint32_t count;
		std::vector<uint8_t> data;
	};

	constexpr uint16_t FMT_STRING = 2;
	constexpr uint16_t FMT_URATIONAL = 5;
	constexpr uint16_t FMT_SRATIONAL = 10;

	const auto ascii = [](const std::string_view s) { return std::vector<uint8_t>(s.begin(), s.end()); };
	const auto rational_bytes = [](const int32_t n, const int32_t d)
	{
		std::vector<uint8_t> v(8);
		memcpy(v.data(), &n, 4);
		memcpy(v.data() + 4, &d, 4);
		return v;
	};

	// Entries must be in ascending tag order (ARTIST before XPAuthor so the XPAuthor
	// guard is proven not to overwrite the already-set artist).
	std::vector<exif_entry> entries = {
		{0x0131, FMT_STRING, 0, ascii("DiffractorApp")}, // Software      -> encoder
		{0x013b, FMT_STRING, 0, ascii("Ansel Adams")}, //   Artist        -> artist
		{0x9201, FMT_SRATIONAL, 1, rational_bytes(6, 1)}, // ShutterSpeed  -> 2^-6 = 1/64s
		{0x9205, FMT_URATIONAL, 1, rational_bytes(4, 1)}, // MaxAperture   -> 2^(4/2) = f/4
		{0x9c9b, FMT_STRING, 0, ascii("Winter")}, //         XPTitle       -> title
		{0x9c9d, FMT_STRING, 0, ascii("Ignored Author")}, // XPAuthor      -> artist (guarded)
		{0x9c9e, FMT_STRING, 0, ascii("alpha beta")}, //     XPKeywords    -> tags
		{0x9c9f, FMT_STRING, 0, ascii("A subject")}, //      XPSubject     -> description
	};

	for (auto& e : entries)
	{
		if (e.count == 0) e.count = static_cast<uint32_t>(e.data.size());
	}

	std::vector<uint8_t> buf;
	const auto put16 = [&buf](const uint16_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
	};
	const auto put32 = [&buf](const uint32_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
		buf.push_back(static_cast<uint8_t>(v >> 16));
		buf.push_back(static_cast<uint8_t>(v >> 24));
	};

	const auto entry_count = static_cast<uint32_t>(entries.size());
	constexpr uint32_t ifd0_offset = 8;
	const uint32_t data_start = ifd0_offset + 2 + entry_count * 12 + 4;

	// Reserve word-aligned out-of-line offsets for payloads that don't fit in 4 bytes.
	std::vector<uint32_t> data_offset(entries.size(), 0);
	uint32_t cursor = data_start;
	for (size_t i = 0; i < entries.size(); ++i)
	{
		const auto size = static_cast<uint32_t>(entries[i].data.size());
		if (size > 4)
		{
			data_offset[i] = cursor;
			cursor += size;
			if (cursor & 1) ++cursor;
		}
	}

	// TIFF header (little-endian).
	put16(0x4949);
	put16(0x002a);
	put32(ifd0_offset);

	// IFD0.
	put16(static_cast<uint16_t>(entry_count));
	for (size_t i = 0; i < entries.size(); ++i)
	{
		const auto& e = entries[i];
		put16(e.tag);
		put16(e.fmt);
		put32(e.count);

		const auto size = static_cast<uint32_t>(e.data.size());
		if (size > 4)
		{
			put32(data_offset[i]);
		}
		else
		{
			uint32_t inline_value = 0;
			memcpy(&inline_value, e.data.data(), size);
			put32(inline_value);
		}
	}
	put32(0); // No IFD1.

	// Out-of-line payloads (word aligned, matching the reserved offsets above).
	for (const auto& e : entries)
	{
		if (e.data.size() > 4)
		{
			buf.insert(buf.end(), e.data.begin(), e.data.end());
			if (buf.size() & 1) buf.push_back(0);
		}
	}

	prop::item_metadata md;
	metadata_exif::parse(md, df::cspan{buf.data(), buf.size()});

	assert_equal("DiffractorApp", md.encoder, "Software -> encoder");
	assert_equal("Ansel Adams", md.artist, "Artist -> artist (XPAuthor must not override)");
	assert_equal("Winter", md.title, "XPTitle -> title");
	assert_equal("alpha beta", md.tags, "XPKeywords -> tags");
	assert_equal("A subject", md.description, "XPSubject -> description");
	assert_equal(prop::format_f_num(4.0f), prop::format_f_num(md.f_number), "MaxApertureValue -> f_number");
	assert_equal(prop::format_exposure(1.0f / 64.0f), prop::format_exposure(md.exposure_time),
	             "ShutterSpeedValue -> exposure_time");
}

// Windows writes its XP tags as BYTE arrays of UTF-16LE text ending in a NUL. Diffractor's own parser
// read them as "?", which became the title and tags of a file with no XMP to override them.
static void should_read_windows_xp_tags_as_written()
{
	const auto utf16 = [](const std::u16string_view text)
	{
		std::vector<uint8_t> v;
		for (const auto c : text)
		{
			v.push_back(static_cast<uint8_t>(c));
			v.push_back(static_cast<uint8_t>(c >> 8));
		}
		v.push_back(0);
		v.push_back(0);
		return v;
	};

	const std::vector<std::pair<uint16_t, std::vector<uint8_t>>> entries = {
		{0x9c9b, utf16(u"Winter")}, // XPTitle
		{0x9c9e, utf16(u"alpha;beta")}, // XPKeywords
		{0x9c9f, utf16(u"A subj\u00e9ct")}, // XPSubject
	};

	std::vector<uint8_t> buf;
	const auto put16 = [&buf](const uint16_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
	};
	const auto put32 = [&put16](const uint32_t v)
	{
		put16(static_cast<uint16_t>(v));
		put16(static_cast<uint16_t>(v >> 16));
	};

	constexpr uint16_t fmt_byte = 1;
	auto value_at = static_cast<uint32_t>(8 + 2 + entries.size() * 12 + 4);

	put16(0x4949); // little-endian TIFF header
	put16(0x002a);
	put32(8);
	put16(static_cast<uint16_t>(entries.size()));

	for (const auto& [tag, value] : entries)
	{
		put16(tag);
		put16(fmt_byte);
		put32(static_cast<uint32_t>(value.size()));
		put32(value_at);
		value_at += static_cast<uint32_t>(value.size());
	}

	put32(0);
	for (const auto& entry : entries) buf.insert(buf.end(), entry.second.begin(), entry.second.end());

	prop::item_metadata md;
	metadata_exif::parse(md, df::cspan{buf.data(), buf.size()});

	assert_equal("Winter", md.title, "XPTitle -> title");
	assert_equal("alpha;beta", md.tags, "XPKeywords -> tags");
	assert_equal("A subj\xc3\xa9" "ct", md.description, "XPSubject -> description");
}

// locations.md 2.8: GPS altitude and speed live in a sub-IFD reached through tag 0x8825, and
// the altitude reference that decides the sign may arrive either side of the value.
struct test_gps_entry
{
	uint16_t tag;
	uint16_t fmt;
	uint32_t count;
	std::vector<uint8_t> data;
};

// A minimal little-endian TIFF whose IFD0 carries nothing but a GPS IFD pointer, and whose GPS IFD
// carries the given entries. A value wider than four bytes is written after the directory and
// referenced by offset, as the format requires.
static std::vector<uint8_t> build_gps_exif(const std::vector<test_gps_entry>& entries)
{
	std::vector<uint8_t> buf;
	const auto put16 = [&buf](const uint16_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
	};
	const auto put32 = [&buf](const uint32_t v)
	{
		buf.push_back(static_cast<uint8_t>(v));
		buf.push_back(static_cast<uint8_t>(v >> 8));
		buf.push_back(static_cast<uint8_t>(v >> 16));
		buf.push_back(static_cast<uint8_t>(v >> 24));
	};

	constexpr uint32_t ifd0_offset = 8;
	constexpr uint32_t gps_ifd_offset = ifd0_offset + 2 + 12 + 4; // IFD0 holds one entry
	const auto count = static_cast<uint32_t>(entries.size());
	const uint32_t gps_data_start = gps_ifd_offset + 2 + count * 12 + 4;

	std::vector<uint32_t> data_offset(entries.size(), 0);
	uint32_t cursor = gps_data_start;
	for (size_t i = 0; i < entries.size(); ++i)
	{
		if (entries[i].data.size() > 4)
		{
			data_offset[i] = cursor;
			cursor += static_cast<uint32_t>(entries[i].data.size());
		}
	}

	put16(0x4949);
	put16(0x002a);
	put32(ifd0_offset);

	put16(1);
	put16(0x8825); // GPS IFD pointer
	put16(4); // FMT_ULONG
	put32(1);
	put32(gps_ifd_offset);
	put32(0);

	put16(static_cast<uint16_t>(count));
	for (size_t i = 0; i < entries.size(); ++i)
	{
		const auto& e = entries[i];
		put16(e.tag);
		put16(e.fmt);
		put32(e.count);

		const auto size = static_cast<uint32_t>(e.data.size());
		if (size > 4)
		{
			put32(data_offset[i]);
		}
		else
		{
			uint32_t inline_value = 0;
			memcpy(&inline_value, e.data.data(), size);
			put32(inline_value);
		}
	}
	put32(0);

	for (const auto& e : entries)
	{
		if (e.data.size() > 4) buf.insert(buf.end(), e.data.begin(), e.data.end());
	}

	return buf;
}

static std::vector<uint8_t> exif_urational(const uint32_t n, const uint32_t d)
{
	std::vector<uint8_t> v(8);
	memcpy(v.data(), &n, 4);
	memcpy(v.data() + 4, &d, 4);
	return v;
}

static void should_parse_exif_gps_height()
{
	const auto& urational = exif_urational;

	constexpr uint16_t FMT_BYTE = 1;
	constexpr uint16_t FMT_STRING = 2;
	constexpr uint16_t FMT_URATIONAL = 5;

	// A cruising airliner: 10,668 m above sea level at 900 km/h.
	prop::item_metadata cruising;
	const auto cruising_exif = build_gps_exif({
		{0x0005, FMT_BYTE, 1, {0}},
		{0x0006, FMT_URATIONAL, 1, urational(10668, 1)},
		{0x000c, FMT_STRING, 2, {'K', 0}},
		{0x000d, FMT_URATIONAL, 1, urational(900, 1)},
	});
	metadata_exif::parse(cruising, df::cspan{cruising_exif.data(), cruising_exif.size()});
	assert_equal(10668.0f, cruising.altitude, "GPSAltitude -> altitude");
	assert_equal(900.0f, cruising.gps_speed, "GPSSpeed in km/h -> gps_speed");

	// A dive: reference 1 means below sea level, so the stored positive magnitude is negated.
	prop::item_metadata dive;
	const auto dive_exif = build_gps_exif({
		{0x0005, FMT_BYTE, 1, {1}},
		{0x0006, FMT_URATIONAL, 1, urational(180, 10)},
	});
	metadata_exif::parse(dive, df::cspan{dive_exif.data(), dive_exif.size()});
	assert_equal(-18.0f, dive.altitude, "below-sea-level reference negates the altitude");

	// Knots are the other common speed reference.
	prop::item_metadata knots;
	const auto knots_exif = build_gps_exif({
		{0x000c, FMT_STRING, 2, {'N', 0}},
		{0x000d, FMT_URATIONAL, 1, urational(100, 1)},
	});
	metadata_exif::parse(knots, df::cspan{knots_exif.data(), knots_exif.size()});
	assert_equal(185.2f, knots.gps_speed, "GPSSpeed in knots -> km/h");
}

// A coordinate is absent when a component is missing, and +/-180 is the sentinel that says so.
// Testing each magnitude for zero as well threw away the whole equator and the whole Greenwich
// meridian. Zero is a position on both, and only the exact pair - what a receiver with no fix
// writes - is worth refusing.
static void should_keep_gps_on_the_equator_and_meridian()
{
	constexpr uint16_t FMT_BYTE = 1;
	constexpr uint16_t FMT_STRING = 2;
	constexpr uint16_t FMT_URATIONAL = 5;

	// EXIF stores a coordinate as three rationals: degrees, minutes and seconds.
	const auto dms = [](const uint32_t d, const uint32_t m, const uint32_t s)
	{
		std::vector<uint8_t> v;
		for (const auto part : {d, m, s})
		{
			const auto r = exif_urational(part, 1);
			v.insert(v.end(), r.begin(), r.end());
		}
		return v;
	};

	// Nanyuki, Kenya - on the equator, east of Greenwich.
	prop::item_metadata equator;
	const auto equator_exif = build_gps_exif({
		{0x0001, FMT_STRING, 2, {'N', 0}},
		{0x0002, FMT_URATIONAL, 3, dms(0, 0, 0)},
		{0x0003, FMT_STRING, 2, {'E', 0}},
		{0x0004, FMT_URATIONAL, 3, dms(37, 4, 0)},
	});
	metadata_exif::parse(equator, df::cspan{equator_exif.data(), equator_exif.size()});
	assert_equal(true, equator.coordinate.is_valid(), "a latitude of zero is a position, not an absence");
	assert_equal(true, std::abs(equator.coordinate.longitude() - 37.0667) < 0.001, "and it keeps its longitude");

	// Greenwich - zero longitude, well north of the equator.
	prop::item_metadata meridian;
	const auto meridian_exif = build_gps_exif({
		{0x0001, FMT_STRING, 2, {'N', 0}},
		{0x0002, FMT_URATIONAL, 3, dms(51, 28, 40)},
		{0x0003, FMT_STRING, 2, {'E', 0}},
		{0x0004, FMT_URATIONAL, 3, dms(0, 0, 0)},
	});
	metadata_exif::parse(meridian, df::cspan{meridian_exif.data(), meridian_exif.size()});
	assert_equal(true, meridian.coordinate.is_valid(), "a longitude of zero is a position too");
	assert_equal(true, std::abs(meridian.coordinate.latitude() - 51.4778) < 0.001, "and it keeps its latitude");

	// Null Island is still refused: a receiver with no fix writes exactly this.
	prop::item_metadata no_fix;
	const auto no_fix_exif = build_gps_exif({
		{0x0001, FMT_STRING, 2, {'N', 0}},
		{0x0002, FMT_URATIONAL, 3, dms(0, 0, 0)},
		{0x0003, FMT_STRING, 2, {'E', 0}},
		{0x0004, FMT_URATIONAL, 3, dms(0, 0, 0)},
	});
	metadata_exif::parse(no_fix, df::cspan{no_fix_exif.data(), no_fix_exif.size()});
	assert_equal(false, no_fix.coordinate.is_valid(), "but a zero pair is a receiver with no fix");

	// And a file carrying no coordinate at all still has none.
	prop::item_metadata none;
	const auto none_exif = build_gps_exif({{0x0005, FMT_BYTE, 1, {0}}});
	metadata_exif::parse(none, df::cspan{none_exif.data(), none_exif.size()});
	assert_equal(false, none.coordinate.is_valid(), "and an absent coordinate stays absent");
}

// IIM 4.1 encodes a dataset as 0x1C, record, dataset, then a two-octet size field. With the field's
// top bit set the low 15 bits are not the size - they are the count of octets that carry it, and
// those follow. Reading the size out of the field itself took the wrong bytes and advanced by the
// wrong amount, so the walk lost sync and every dataset after an extended one was read out of a
// payload rather than a header.
static void should_parse_an_extended_length_iptc_dataset()
{
	// One dataset. `size_octets` of 0 asks for the ordinary two-octet form.
	const auto dataset = [](const uint8_t record, const uint8_t tag, const std::string_view value,
	                        const uint32_t size_octets)
	{
		std::vector<uint8_t> b{0x1c, record, tag};

		if (size_octets == 0)
		{
			b.push_back(static_cast<uint8_t>(value.size() >> 8));
			b.push_back(static_cast<uint8_t>(value.size()));
		}
		else
		{
			b.push_back(static_cast<uint8_t>(0x80 | (size_octets >> 8)));
			b.push_back(static_cast<uint8_t>(size_octets));

			for (auto i = size_octets; i-- > 0;)
			{
				b.push_back(static_cast<uint8_t>(value.size() >> (8 * i)));
			}
		}

		b.insert(b.end(), value.begin(), value.end());
		return b;
	};

	constexpr uint8_t application_record = 2;
	constexpr uint8_t object_name = 5;
	constexpr uint8_t copyright = 116;

	{
		// The ordinary form still reads, and a dataset after it is still found.
		std::vector<uint8_t> block;
		const auto a = dataset(application_record, object_name, "Lisbon", 0);
		const auto b = dataset(application_record, copyright, "Ada Lovelace", 0);
		block.insert(block.end(), a.begin(), a.end());
		block.insert(block.end(), b.begin(), b.end());

		prop::item_metadata md;
		metadata_iptc::parse(md, df::cspan{block.data(), block.size()});
		assert_equal("Lisbon", md.title, "a two-octet size still reads");
		assert_equal("Ada Lovelace", md.copyright_notice, "and the dataset after it is still found");
	}

	{
		// The extended form, and a following dataset that only parses if the walk stayed in sync.
		std::vector<uint8_t> block;
		const auto a = dataset(application_record, object_name, "Lisbon", 4);
		const auto b = dataset(application_record, copyright, "Ada Lovelace", 0);
		block.insert(block.end(), a.begin(), a.end());
		block.insert(block.end(), b.begin(), b.end());

		prop::item_metadata md;
		metadata_iptc::parse(md, df::cspan{block.data(), block.size()});
		assert_equal("Lisbon", md.title, "an extended size reads its own payload");
		assert_equal("Ada Lovelace", md.copyright_notice, "and the walk stays aligned for what follows");
	}
}

// Issue #65 - Binary text in JPEG comment
// Some Samsung phones (e.g. SM-G900F) store a binary blob in the EXIF UserComment
// tag, either raw or behind a valid "ASCII\0\0\0" character code. The parser must
// recognise the junk and drop it, while still preserving valid comments.
static void should_drop_binary_exif_comment()
{
	constexpr uint16_t TAG_USER_COMMENT = 0x9286;
	constexpr uint16_t FMT_UNDEFINED = 7;

	// Builds a minimal little-endian TIFF/EXIF block containing a single entry.
	const auto build_exif = [](const uint16_t tag, const uint16_t fmt, const std::vector<uint8_t>& data)
	{
		std::vector<uint8_t> buf;
		const auto put16 = [&buf](const uint16_t v)
		{
			buf.push_back(static_cast<uint8_t>(v));
			buf.push_back(static_cast<uint8_t>(v >> 8));
		};
		const auto put32 = [&buf](const uint32_t v)
		{
			buf.push_back(static_cast<uint8_t>(v));
			buf.push_back(static_cast<uint8_t>(v >> 8));
			buf.push_back(static_cast<uint8_t>(v >> 16));
			buf.push_back(static_cast<uint8_t>(v >> 24));
		};

		constexpr uint32_t ifd0_offset = 8;
		constexpr uint32_t data_start = ifd0_offset + 2 + 1 * 12 + 4; // one entry
		const auto size = static_cast<uint32_t>(data.size());

		put16(0x4949); // little-endian TIFF header
		put16(0x002a);
		put32(ifd0_offset);

		put16(1); // entry count
		put16(tag);
		put16(fmt);
		put32(size);
		if (size > 4)
		{
			put32(data_start);
		}
		else
		{
			uint32_t inline_value = 0;
			memcpy(&inline_value, data.data(), size);
			put32(inline_value);
		}
		put32(0); // no IFD1

		if (size > 4) buf.insert(buf.end(), data.begin(), data.end());
		return buf;
	};

	// The Samsung junk blob begins with the fixed marker {0x12, 0xf8, 0x0f, 0x3b}.
	const std::vector<uint8_t> junk = {
		0x12, 0xf8, 0x0f, 0x3b, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77
	};
	prop::item_metadata md_junk;
	const auto exif_junk = build_exif(TAG_USER_COMMENT, FMT_UNDEFINED, junk);
	metadata_exif::parse(md_junk, df::cspan{exif_junk.data(), exif_junk.size()});
	assert_equal(true, prop::is_null(md_junk.comment), "binary Samsung comment dropped");

	// SM-G900F hides its blob behind a valid "ASCII\0\0\0" character code, so the marker
	// test alone never fires; the payload is still binary and must be dropped.
	std::vector<uint8_t> samsung = {'A', 'S', 'C', 'I', 'I', 0, 0, 0, 0x0a, 0, 0, 0};
	for (const auto c : {'J', 'K', 'J', 'K'}) samsung.push_back(static_cast<uint8_t>(c));
	for (const uint8_t c : {0x27, 0x03, 0xab, 0x5c, 0x46, 0x0b, 0x01, 0x00}) samsung.push_back(c);
	prop::item_metadata md_samsung;
	const auto exif_samsung = build_exif(TAG_USER_COMMENT, FMT_UNDEFINED, samsung);
	metadata_exif::parse(md_samsung, df::cspan{exif_samsung.data(), exif_samsung.size()});
	assert_equal(true, prop::is_null(md_samsung.comment), "ASCII-prefixed binary comment dropped");

	// Control: a well-formed ASCII UserComment ("ASCII\0\0\0" prefix) is preserved.
	const std::vector<uint8_t> good = {
		'A', 'S', 'C', 'I', 'I', 0, 0, 0, 'H', 'e', 'l', 'l', 'o'
	};
	prop::item_metadata md_good;
	const auto exif_good = build_exif(TAG_USER_COMMENT, FMT_UNDEFINED, good);
	metadata_exif::parse(md_good, df::cspan{exif_good.data(), exif_good.size()});
	assert_equal("Hello", md_good.comment, "valid ASCII comment preserved");

	// Control: multi-line text with trailing nul padding survives the binary scan.
	const std::vector<uint8_t> padded = {
		'A', 'S', 'C', 'I', 'I', 0, 0, 0, 'L', 'i', 'n', 'e', '\n', '2', 0, 0
	};
	prop::item_metadata md_padded;
	const auto exif_padded = build_exif(TAG_USER_COMMENT, FMT_UNDEFINED, padded);
	metadata_exif::parse(md_padded, df::cspan{exif_padded.data(), exif_padded.size()});
	assert_equal("Line\n2", md_padded.comment, "multi-line padded comment preserved");
}

static void should_copy_preserve_properties()
{
	prop::item_metadata src;
	src.title = "Test Title"_c;
	src.rating = 4;
	src.media_position = 19.5;
	src.iso_speed = 400;
	src.artist = "Test Artist"_c;

	prop::item_metadata dst;
	dst = src;

	assert_equal(src.title, dst.title, "should copy title");
	assert_equal(src.rating, dst.rating, "should copy rating");
	assert_equal(src.iso_speed, dst.iso_speed, "should copy iso_speed");
	assert_equal(src.artist, dst.artist, "should copy artist");
}

static void should_replace_item_metadata_without_resetting_playback_position()
{
	df::index_file_item initial;
	initial.name = "position.mp4"_c;
	initial.ft = files::file_type_from_name(initial.name);
	const auto initial_metadata = std::make_shared<prop::item_metadata>();
	initial_metadata->title = "Initial"_c;
	initial_metadata->media_position = 12.0;
	initial.metadata.store(initial_metadata);

	const auto path = df::file_path("c:\\position.mp4");
	const auto item = std::make_shared<df::item_element>(path, initial);
	const auto old_snapshot = item->metadata();
	item->media_position(24.0);

	const auto refreshed = initial;
	const auto refreshed_metadata = std::make_shared<prop::item_metadata>(*initial_metadata);
	refreshed_metadata->title = "Refreshed"_c;
	refreshed_metadata->media_position = 4.0;
	refreshed.metadata.store(refreshed_metadata);
	item->update(path, refreshed);

	assert_equal("Initial", old_snapshot->title, "old metadata snapshot remains unchanged");
	assert_equal("Refreshed", item->metadata()->title, "item receives refreshed metadata snapshot");
	assert_equal(24, static_cast<int>(item->media_position()), "live playback position survives metadata refresh");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #209 - Unable to remove tags by any means
// Tags removed via tag_set::remove() should actually be deleted.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_remove_tags()
{
	tag_set tags("cat dog bird fish");
	assert_equal(4, static_cast<int>(tags.size()), "initial tag count");

	// Remove a single tag
	const tag_set to_remove("dog");
	tags.remove(to_remove);
	assert_equal(3, static_cast<int>(tags.size()), "after removing dog");

	// Verify removed tag is gone and others remain
	const auto result = tags.to_string();
	assert_equal(true, result.find("dog") == std::string::npos, "dog should be gone");
	assert_equal(true, result.find("cat") != std::string::npos, "cat should remain");
	assert_equal(true, result.find("bird") != std::string::npos, "bird should remain");
	assert_equal(true, result.find("fish") != std::string::npos, "fish should remain");
}

static void should_remove_tags_case_insensitive()
{
	tag_set tags("Cat Dog Bird");
	const tag_set to_remove("DOG");
	tags.remove(to_remove);

	assert_equal(2, static_cast<int>(tags.size()), "case insensitive remove");
	assert_equal(true, tags.to_string().find("Dog") == std::string::npos, "Dog gone");
}

static void should_remove_multiple_tags()
{
	tag_set tags("cat dog bird fish");
	const tag_set to_remove("dog fish");
	tags.remove(to_remove);

	assert_equal(2, static_cast<int>(tags.size()), "multi-remove count");
}

static void should_remove_all_tags()
{
	tag_set tags("cat dog");
	const tag_set to_remove("cat dog");
	tags.remove(to_remove);

	assert_equal(true, tags.is_empty(), "all tags removed");
}

static void should_remove_nonexistent_tag()
{
	tag_set tags("cat dog");
	const tag_set to_remove("elephant");
	tags.remove(to_remove);

	// Should not crash and should leave existing tags untouched
	assert_equal(2, static_cast<int>(tags.size()), "remove nonexistent");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #219 - Korean tags are not working
// Korean (Hangul) tags must round-trip through tag parsing.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_parse_korean_tags()
{
	constexpr auto family = "\uAC00\uC871"; // 가족
	constexpr auto travel = "\uC5EC\uD589"; // 여행
	constexpr auto photo = "\uC0AC\uC9C4"; // 사진

	// Space-separated Korean tags split into individual tags.
	tag_set tags(std::format("{} {} {}", family, travel, photo));
	assert_equal(3, static_cast<int>(tags.size()), "korean tag count");

	// Removing one Korean tag leaves the others intact (case-insensitive path).
	tags.remove(tag_set(travel));
	assert_equal(2, static_cast<int>(tags.size()), "korean tag count after remove");
	assert_equal(true, tags.to_string().find(travel) == std::string::npos, "travel tag removed");
	assert_equal(true, tags.to_string().find(family) != std::string::npos, "family tag remains");
	assert_equal(true, tags.to_string().find(photo) != std::string::npos, "photo tag remains");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Issue #184 - 'Group by date created' uses the wrong date (ignores DateTimeOriginal)
// The one date an item shows must be its capture time when it has one, and the container date
// only when it does not.
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_prefer_datetimeoriginal_for_created()
{
	prop::item_metadata md;
	md.dates.add_utc(prop::date_source::container_created, df::date_t(2020, 1, 1, 12, 0, 0));
	md.dates.add(prop::date_source::exif_original, df::date_t(2005, 6, 15, 9, 30, 0));
	assert_equal(df::date_t(2005, 6, 15, 9, 30, 0), md.created(), "prefers DateTimeOriginal");

	// With no capture time, fall back to the container date - converted, because it is an instant.
	prop::item_metadata md2;
	md2.dates.add_utc(prop::date_source::container_created, df::date_t(2020, 1, 1, 12, 0, 0));
	assert_equal(df::date_t(2020, 1, 1, 12, 0, 0).system_to_local(), md2.created(), "falls back to the container date");

	// With neither set the date is invalid (the item then uses the file date).
	const prop::item_metadata md3;
	assert_equal(false, md3.created().is_valid(), "no dates -> invalid");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The date pack - docs/metadata.md#dates
// Three dates resolved from many tags by one authority table. These tests own the rules that
// table encodes, because every other date behaviour in the application reads through it.
///////////////////////////////////////////////////////////////////////////////////////////////////

// resolve walks date_sources in table order and takes the first source the file supplied, so the
// table order IS the precedence rule. A row inserted in the wrong place would silently repoint
// every date in the application, and nothing else would fail.
static void should_hold_date_sources_in_authority_order()
{
	uint64_t seen = 0;
	auto kind_count = 0;

	for (auto kind : {
		     prop::date_concept::original, prop::date_concept::created,
		     prop::date_concept::modified, prop::date_concept::reference
	     })
	{
		auto previous_authority = 0;
		auto found = 0;

		for (const auto& info : prop::date_sources)
		{
			if (info.kind != kind) continue;

			assert_equal(previous_authority + 1, static_cast<int>(info.authority),
			             "authority runs 1..n in table order");
			previous_authority = info.authority;
			++found;

			const auto bit = static_cast<uint64_t>(info.source);
			assert_equal(0ull, seen & bit, "each source appears exactly once");
			assert_equal(true, bit != 0 && (bit & (bit - 1)) == 0, "each source is one bit");
			seen |= bit;
		}

		assert_equal(true, found > 0, "every kind has at least one source");
		++kind_count;
	}

	assert_equal(4, kind_count, "four kinds of date");
	assert_equal(static_cast<int>(std::size(prop::date_sources)), std::popcount(seen),
	             "every table row contributes a distinct source");
}

// The same instant arrives whole-second and unzoned from EXIF, and with milliseconds and an offset
// from XMP. If those are two values the pack stores six dates for a phone photograph and the
// tooltip claims disagreement where there is none.
static void should_collapse_sources_that_agree_to_the_second()
{
	prop::date_pack pack;
	pack.add(prop::date_source::exif_original, df::date_t(2025, 8, 16, 18, 11, 56));
	pack.add(prop::date_source::exif_digitized, df::date_t(2025, 8, 16, 18, 11, 56));

	// The XMP reading of the same second, carrying 368 ms and a +01:00 offset.
	const auto with_millis = df::date_t(df::date_t(2025, 8, 16, 18, 11, 56).to_int64() +
		368 * (df::date_t::intervals_per_second / 1000));
	pack.add(prop::date_source::xmp_create, with_millis, 60);

	assert_equal(1, pack.group_count(), "one value, not three");
	assert_equal(with_millis, pack.group_value(0), "the more precise reading is kept");
	assert_equal(60, static_cast<int>(pack.group_offset(0)), "the offset the zoned source supplied is kept");
	assert_equal(true, pack.has_source(prop::date_source::exif_original), "every agreeing source is recorded");
	assert_equal(true, pack.has_source(prop::date_source::xmp_create), "every agreeing source is recorded");
}

// #184 - an image editor rewrites photoshop:DateCreated on save, so a retouched photograph carries
// one ten months after its capture. It may never outrank the shutter.
static void should_rank_editor_rewritten_dates_below_the_shutter()
{
	prop::date_pack pack;
	pack.add(prop::date_source::photoshop_created, df::date_t(2026, 6, 2, 7, 22, 9));
	pack.add(prop::date_source::exif_original, df::date_t(2025, 8, 16, 18, 11, 56));

	assert_equal(df::date_t(2025, 8, 16, 18, 11, 56), pack.original(), "DateTimeOriginal wins");
	assert_equal(true, pack.resolved_source(prop::date_concept::original) == prop::date_source::exif_original,
	             "and the panel can say so");

	// With nothing better it is still the best answer available, which is why it is read at all.
	prop::date_pack only_editor;
	only_editor.add(prop::date_source::photoshop_created, df::date_t(2026, 6, 2, 7, 22, 9));
	assert_equal(df::date_t(2026, 6, 2, 7, 22, 9), only_editor.original(), "used when nothing else answers");
}

// #184 - EXIF 0x0132 is the time the file last changed. Reading it as a creation date is what put
// an edited photograph under the day of its edit, and IFD0 is walked first so order cannot save us.
static void should_read_exif_datetime_as_modified()
{
	prop::date_pack pack;
	pack.add(prop::date_source::exif_datetime, df::date_t(2023, 8, 1, 0, 0, 0));
	pack.add(prop::date_source::exif_original, df::date_t(1990, 2, 1, 0, 0, 0));

	assert_equal(df::date_t(1990, 2, 1, 0, 0, 0), pack.original(), "capture time");
	assert_equal(df::date_t(2023, 8, 1, 0, 0, 0), pack.modified(), "0x0132 is the file change time");
	assert_equal(false, pack.created().is_valid(), "and it is not a creation date");
	assert_equal(df::date_t(1990, 2, 1, 0, 0, 0), pack.best(), "so the item files under 1990");
}

// Modified describes the file, so the filesystem answers it. A metadata modify tag records when a
// tool last wrote the metadata: a copy preserves it and an editor that does not maintain it never
// touches it, so it is stale exactly when the user is asking. Nothing fills the filesystem rung
// during a scan - the stamps belong to the index record, and `modified:` reads them directly - so
// this pins the ranking against the moment something does, which is what the bit exists for.
static void should_take_the_modified_date_from_the_filesystem()
{
	prop::date_pack pack;
	pack.add(prop::date_source::exif_datetime, df::date_t(2019, 5, 4, 9, 0, 0));
	pack.add(prop::date_source::xmp_modify, df::date_t(2019, 5, 4, 9, 0, 0));
	pack.add(prop::date_source::file_modified, df::date_t(2026, 8, 19, 21, 30, 0));

	assert_equal(df::date_t(2026, 8, 19, 21, 30, 0), pack.modified(), "the stamp, not the stale tag");
	assert_equal("File modified", prop::date_source_name(pack.resolved_source(prop::date_concept::modified)),
	             "and it says so");

	// Copying destroys the creation stamp and preserves the modification stamp, which is why the
	// two concepts rank the filesystem differently rather than by accident.
	prop::date_pack copied;
	copied.add(prop::date_source::container_created, df::date_t(2020, 1, 2, 3, 4, 5));
	copied.add(prop::date_source::file_created, df::date_t(2026, 8, 19, 21, 30, 0));

	assert_equal(df::date_t(2020, 1, 2, 3, 4, 5), copied.created(), "the container, not the copy time");

	// With no stamp - a remote item, or a placeholder that was never hydrated - the tag still answers.
	prop::date_pack no_stamp;
	no_stamp.add(prop::date_source::exif_datetime, df::date_t(2019, 5, 4, 9, 0, 0));
	assert_equal(df::date_t(2019, 5, 4, 9, 0, 0), no_stamp.modified(), "rather than nothing at all");
}

// #184 - the date bubble names the tag behind each date, then lists the filesystem stamps. A stamp
// that already answered one of the three named dates must not be repeated under its own name, or
// the bubble shows the same instant twice and reads as a contradiction.
static void should_not_repeat_a_filesystem_stamp_in_the_date_bubble()
{
	// The ordinary photograph: metadata answers Original and Created, the stamp answers Modified.
	prop::date_pack photo;
	photo.add(prop::date_source::exif_original, df::date_t(2025, 8, 16, 18, 11, 56));
	photo.add(prop::date_source::xmp_create, df::date_t(2025, 8, 16, 18, 12, 30));
	photo.add(prop::date_source::file_modified, df::date_t(2026, 8, 19, 21, 30, 0));

	assert_equal(true, prop::shows_own_file_stamp_row(photo, prop::date_source::file_created),
	             "the creation stamp answered nothing, so it gets its own row");
	assert_equal(false, prop::shows_own_file_stamp_row(photo, prop::date_source::file_modified),
	             "the modification stamp is already the Modified row");

	// A file carrying no metadata dates at all: both stamps are rows of their own, and they are the
	// only rows there are.
	prop::date_pack bare;
	assert_equal(true, prop::shows_own_file_stamp_row(bare, prop::date_source::file_created),
	             "nothing above claimed the creation stamp");
	assert_equal(true, prop::shows_own_file_stamp_row(bare, prop::date_source::file_modified),
	             "nothing above claimed the modification stamp");

	// A copy: the creation stamp is the copy time and the container still states when the content
	// was made, so Created is answered by the container and the stamp stands on its own.
	prop::date_pack copied;
	copied.add(prop::date_source::container_created, df::date_t(2020, 1, 2, 3, 4, 5));
	copied.add(prop::date_source::file_created, df::date_t(2026, 8, 19, 21, 30, 0));
	assert_equal(true, prop::shows_own_file_stamp_row(copied, prop::date_source::file_created),
	             "the container answered Created, so the copy time is still worth showing");

	// The filesystem stamps are read from the item rather than stored with the metadata, so in
	// practice the pack does not carry them and asking which tag won cannot answer on its own. A row
	// carrying an instant already on screen under another tag is the duplicate this exists to stop.
	prop::date_pack unstamped;
	unstamped.add(prop::date_source::exif_original, df::date_t(2025, 8, 16, 18, 11, 56));
	unstamped.add(prop::date_source::xmp_modify, df::date_t(2026, 8, 19, 21, 30, 0));

	assert_equal(false, prop::shows_own_file_stamp_row(unstamped, prop::date_source::file_modified,
	                                                   df::date_t(2026, 8, 19, 21, 30, 0)),
	             "the same instant under another tag is the same row");
	assert_equal(true, prop::shows_own_file_stamp_row(unstamped, prop::date_source::file_modified,
	                                                  df::date_t(2026, 8, 19, 21, 31, 0)),
	             "a minute apart is a different fact and says something");
	assert_equal(true, prop::shows_own_file_stamp_row(unstamped, prop::date_source::file_created,
	                                                  df::date_t(2026, 8, 19, 22, 0, 0)),
	             "and a stamp nothing above matches keeps its row");
}

// A zone and a fraction are separate EXIF tags from the date they qualify, and OffsetTime lives in
// a different directory from DateTime, so neither can be applied where it is read. Every one of
// these is stored rather than displayed today: they exist so the release that uses them needs no
// re-index of its own.
static void should_carry_the_zone_and_fraction_a_date_was_written_with()
{
	assert_equal(120, prop::parse_utc_offset("+02:00"), "a bare EXIF offset");
	assert_equal(-450, prop::parse_utc_offset("-07:30"), "west of Greenwich, and not a whole hour");
	assert_equal(120, prop::parse_utc_offset("2019-05-04T09:00:00+02:00"), "the tail of an ISO timestamp");
	assert_equal(prop::date_pack::utc_instant, prop::parse_utc_offset("2019-05-04T09:00:00Z"), "an instant");
	assert_equal(prop::date_pack::no_offset, prop::parse_utc_offset("2019:05:04 09:00:00"), "EXIF states no zone");
	assert_equal(prop::date_pack::no_offset, prop::parse_utc_offset("+0200"), "and a malformed one claims nothing");

	// An Exif ASCII value spans its whole component count, so OffsetTimeOriginal arrives as seven
	// characters, not six. Measuring the tail without trimming reads the zone off by one and finds
	// nothing at all.
	assert_equal(120, prop::parse_utc_offset(std::string_view("+02:00\0", 7)), "NUL-terminated, as Exif stores it");
	assert_equal(-450, prop::parse_utc_offset(std::string_view("-07:30 \0", 8)), "and space-padded besides");

	// EXIF writes the fraction as digits after an implied point, so `5` is half a second, not five
	// of anything. Reading it as an integer would put a burst frame 5 seconds late.
	assert_equal(5'000'000ull, prop::parse_sub_second_intervals("5"), "one digit is tenths");
	assert_equal(1'200'000ull, prop::parse_sub_second_intervals("12"), "two digits are hundredths");
	assert_equal(3'090'000ull, prop::parse_sub_second_intervals("309"), "three digits are milliseconds");
	assert_equal(0ull, prop::parse_sub_second_intervals(""), "an absent fraction adds nothing");
	assert_equal(0ull, prop::parse_sub_second_intervals("x"), "and neither does a malformed one");

	// The fraction is what keeps two frames of a burst apart; without it they collapse to one group
	// and order by name instead of by time.
	prop::date_pack burst;
	burst.add(prop::date_source::exif_original, df::date_t(df::date_t(2024, 6, 1, 12, 0, 0).to_int64() + 2'000'000));
	burst.add(prop::date_source::xmp_exif_original, df::date_t(2024, 6, 1, 12, 0, 0));
	assert_equal(1, burst.group_count(), "the same second is one group");
	assert_equal(2'000'000ll, burst.group_value(0).to_int64() % df::date_t::intervals_per_second,
	             "and it keeps the more precise reading");
}

// GPSDateStamp is `CCYY:MM:DD` and nothing else - no time, and colons where the shared parser's
// date-only form expects hyphens. Handing it over as it is read parses nothing, so the GPS instant
// was recorded for no file at all: the rung was filled and dead at the same time.
static void should_read_a_gps_date_stamp_as_a_date()
{
	df::date_t bare;
	assert_equal(false, bare.parse_exif_date("2023:07:14"),
	             "the colon spelling has no date-only form in the shared parser");

	df::date_t punctuated;
	assert_equal(true, punctuated.parse_exif_date("2023:07:14 00:00:00"),
	             "so the stamp is punctuated to midnight before parsing, as IPTC dates already are");
	assert_equal(df::date_t(2023, 7, 14, 0, 0, 0), punctuated, "and it reads as the day the file states");

	// The stamp is only half of it: the pair becomes one UTC instant, which is what makes it able to
	// recover a zone an EXIF reading never stated.
	constexpr auto seconds = 9ull * 3600 + 41 * 60 + 6;
	const df::date_t instant(punctuated.to_int64() + seconds * df::date_t::intervals_per_second);

	prop::date_pack pack;
	pack.add_utc(prop::date_source::gps_stamp, instant);

	assert_equal(true, pack.has_source(prop::date_source::gps_stamp), "the reference date is recorded");
	assert_equal(df::date_t(2023, 7, 14, 9, 41, 6), pack.group_value(0), "with the time the stamp carries");
	assert_equal(prop::date_pack::utc_instant, pack.group_offset(0), "and as an instant, not a wall clock");
}

// A print made in 1980 and scanned in 2024 is the case the three dates exist for: both are true,
// they are decades apart, and the collection must file it under the older one.
static void should_separate_original_from_created_for_a_scan()
{
	prop::date_pack pack;
	pack.add(prop::date_source::exif_original, df::date_t(1980, 6, 1, 12, 0, 0));
	pack.add(prop::date_source::exif_digitized, df::date_t(2024, 3, 9, 16, 45, 0));
	pack.add(prop::date_source::file_modified, df::date_t(2024, 3, 9, 16, 50, 0));

	assert_equal(df::date_t(1980, 6, 1, 12, 0, 0), pack.original(), "when the photograph was taken");
	assert_equal(df::date_t(2024, 3, 9, 16, 45, 0), pack.created(), "when the file was made");
	assert_equal(df::date_t(2024, 3, 9, 16, 50, 0), pack.modified(), "when the file last changed");
	assert_equal(df::date_t(1980, 6, 1, 12, 0, 0), pack.best(), "the item files under 1980");
}

// A scan carrying no capture time must still land somewhere a user can find it.
static void should_fall_back_through_created_to_modified()
{
	prop::date_pack created_only;
	created_only.add(prop::date_source::container_created, df::date_t(2024, 3, 9, 16, 45, 0));
	assert_equal(df::date_t(2024, 3, 9, 16, 45, 0), created_only.best(), "falls back to created");

	prop::date_pack modified_only;
	modified_only.add(prop::date_source::file_modified, df::date_t(2024, 3, 9, 16, 50, 0));
	assert_equal(df::date_t(2024, 3, 9, 16, 50, 0), modified_only.best(), "falls back to modified");

	constexpr prop::date_pack empty;
	assert_equal(false, empty.best().is_valid(), "and claims nothing when the file carries no date");
	assert_equal(true, empty.is_empty(), "an empty pack is empty");
}

// Parse order is an accident of which parser ran first. Two files carrying the same dates must
// produce the same pack, or an unchanged file dirties its database row on every re-scan.
static void should_pack_dates_canonically_regardless_of_parse_order()
{
	prop::date_pack forward;
	forward.add(prop::date_source::exif_original, df::date_t(1980, 6, 1, 12, 0, 0));
	forward.add(prop::date_source::exif_digitized, df::date_t(2024, 3, 9, 16, 45, 0));

	prop::date_pack reverse;
	reverse.add(prop::date_source::exif_digitized, df::date_t(2024, 3, 9, 16, 45, 0));
	reverse.add(prop::date_source::exif_original, df::date_t(1980, 6, 1, 12, 0, 0));

	assert_equal(true, forward == reverse, "the pack is canonical");
	assert_equal(df::date_t(1980, 6, 1, 12, 0, 0), forward.group_value(0), "groups run oldest first");

	// A source with no date says nothing, and must not erase one already recorded.
	auto with_empty = forward;
	with_empty.add(prop::date_source::xmp_create, {});
	assert_equal(true, forward == with_empty, "an absent date changes nothing");
}

// SRC-035 - equal current-machine wall clocks do not prove the same stored representation. A
// floating capture date and a UTC file-created instant can display alike today but must not collapse
// into one group, or changing timezone later shifts the wrong concept and compatibility storage
// loses the instant.
static void should_preserve_date_representation_when_coalescing_sources()
{
	const auto utc_created = df::date_t(2024, 3, 9, 8, 0, 0);
	const auto floating_original = utc_created.system_to_local();

	prop::date_pack forward;
	forward.add(prop::date_source::exif_original, floating_original);
	forward.add_utc(prop::date_source::container_created, utc_created);

	prop::date_pack reverse;
	reverse.add_utc(prop::date_source::container_created, utc_created);
	reverse.add(prop::date_source::exif_original, floating_original);

	for (const auto& pack : {forward, reverse})
	{
		assert_equal(2, pack.group_count(), "floating and UTC representations stay distinct");
		assert_equal(floating_original, pack.original(), "the original remains a floating wall clock");
		assert_equal(floating_original, pack.created(), "the created instant displays as local time");
		assert_equal(utc_created, pack.created_as_utc_instant(), "compatibility storage keeps the instant");
		assert_equal(true, pack.has_source(prop::date_source::exif_original), "original source retained");
		assert_equal(true, pack.has_source(prop::date_source::container_created), "created source retained");
	}
}

// Overflow only ever evicts the lowest-authority distinct values, so no resolution can change -
// but the sources are recorded rather than dropped, so the panel can say how many it did not keep.
static void should_record_dates_that_do_not_fit()
{
	prop::date_pack pack;
	pack.add(prop::date_source::exif_original, df::date_t(2001, 1, 1));
	pack.add(prop::date_source::exif_digitized, df::date_t(2002, 1, 1));
	pack.add(prop::date_source::xmp_create, df::date_t(2003, 1, 1));
	pack.add(prop::date_source::exif_datetime, df::date_t(2004, 1, 1));
	pack.add(prop::date_source::rip_date, df::date_t(2005, 1, 1));

	assert_equal(prop::date_pack::max_groups, pack.group_count(), "the pack is full");
	assert_equal(true, pack.overflow() && prop::date_source::rip_date, "the source that did not fit is recorded");
	assert_equal(true, pack.has_source(prop::date_source::rip_date), "and is still reported as present");
	assert_equal(df::date_t(2001, 1, 1), pack.original(), "the dates that resolve are unaffected");
	assert_equal(df::date_t(2002, 1, 1), pack.created(), "the dates that resolve are unaffected");
	assert_equal(df::date_t(2004, 1, 1), pack.modified(), "the dates that resolve are unaffected");
}

// tag_set is what every keyword edit passes through, and it is the reason a written file keeps its
// tags in a stable order. Ordering, case-insensitive de-duplication and the quoting round trip are
// each load-bearing: a set that reorders makes an unchanged file look edited to the write path.
static void should_combine_and_deduplicate_tag_sets()
{
	tag_set tags("holiday beach");
	assert_equal(2, static_cast<int>(tags.size()), "parsed tag count");

	tags.add(tag_set("Beach sunset"));
	tags.make_unique();

	// Case-insensitive: "Beach" must not join "beach" as a second tag.
	assert_equal(3, static_cast<int>(tags.size()), "a differently cased duplicate is not a new tag");
	assert_equal("beach holiday sunset", tags.to_string(" ", false), "tags are held in name order");

	tags.remove(tag_set("HOLIDAY"));
	assert_equal("beach sunset", tags.to_string(" ", false), "removal ignores case");

	tags.remove(tag_set("absent"));
	assert_equal("beach sunset", tags.to_string(" ", false), "removing an absent tag changes nothing");

	// A tag containing a space has to survive the string round trip, which is what quoting is for.
	tag_set quoted("\"new york\" paris");
	assert_equal(2, static_cast<int>(quoted.size()), "a quoted multi-word tag is one tag");
	assert_equal(true, tag_set(quoted.to_string()) == quoted, "a quoted tag set round trips through text");

	tags.clear();
	assert_equal(true, tags.is_empty(), "a cleared set is empty");
}

// ------------------------------------------------------------------------------------------------
// XMP over the fixtures. Until 2026-10-22 these tests held libfiles to the XMP Toolkit it replaced:
// every packet the fixtures hold parsed to the same tree in both, whole and cut short, and serialized
// byte for byte alike. With the toolkit gone they hold libfiles to itself over the same packets: each
// one, whole or cut short, parses or is refused with libfiles' own error, and what libfiles writes
// reads back as the tree it was written from. Set DIFFRACTOR_XMP_CORPUS to more folders, separated by
// semicolons, to sweep more files than the fixtures.

#include "libfiles/write.h"

namespace xmp_corpus
{
	// The option bits a tree reports; libfiles' internal bits are not part of it.
	constexpr libfiles::xmp::option_bits reported_options = 0x80001FF2;

	// A prefix made unique is numbered by how many clashing URIs the process has met, which differs
	// between a first reading and a reread.
	std::string without_prefix_numbers(std::string text)
	{
		static const std::regex numbered(R"(_\d+_([:=]))");
		return std::regex_replace(text, numbered, "_N_$1");
	}

	std::string dump(const libfiles::xmp::meta& meta)
	{
		auto out = std::format("about={}\n", meta.about());

		for (const auto& p : meta.properties())
		{
			out += std::format("{}|{}|{}|{:x}\n", p.schema, p.path, p.value, p.options & reported_options);
		}

		return without_prefix_numbers(std::move(out));
	}

	struct outcome
	{
		bool refused = false;
		std::string unexpected;
		std::string dump;
	};

	// A packet parses, or libfiles refuses it with its own error; anything else escaping is a fault.
	outcome parse(const std::string_view packet)
	{
		outcome result;

		try
		{
			libfiles::xmp::meta meta;
			meta.parse(packet);
			result.dump = dump(meta);
		}
		catch (const libfiles::xmp::error&)
		{
			result.refused = true;
		}
		catch (const std::exception& e)
		{
			result.unexpected = e.what();
		}
		catch (...)
		{
			result.unexpected = "an exception that is not a std::exception";
		}

		return result;
	}

	// The compact form writes simple properties as attributes, ahead of the elements, so a reread lists
	// the same properties in another order; what it must keep then is each property.
	std::string by_property(const std::string& dump)
	{
		auto lines = str::split(dump, false, [](const char c) { return c == '\n'; });
		std::ranges::sort(lines);

		std::string out;

		for (const auto line : lines)
		{
			out += line;
			out += '\n';
		}

		return out;
	}

	std::string first_difference(const std::string& expected, const std::string& actual)
	{
		const auto e = str::split(expected, false, [](const char c) { return c == '\n'; });
		const auto a = str::split(actual, false, [](const char c) { return c == '\n'; });

		for (size_t i = 0; i < std::max(e.size(), a.size()); ++i)
		{
			const auto el = i < e.size() ? std::string(e[i]) : "<end>"s;
			const auto al = i < a.size() ? std::string(a[i]) : "<end>"s;
			if (el != al) return std::format("written '{}', reread '{}'", el, al);
		}

		return {};
	}

	void add_folder(const df::folder_path folder, std::vector<df::file_path>& files)
	{
		const auto contents = platform::iterate_file_items(folder, false);

		for (const auto& f : contents.files)
		{
			files.push_back(f.folder.combine_file(f.name));
		}

		for (const auto& sub : contents.folders)
		{
			add_folder(folder.combine(sub.name), files);
		}
	}

	std::string environment_variable(const char* const name)
	{
#ifdef _MSC_VER
		char* value = nullptr;
		size_t size = 0;
		if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
		std::string result(value);
		free(value);
		return result;
#else
		const auto* const value = std::getenv(name);
		return value ? value : "";
#endif
	}

	std::vector<df::file_path> corpus_files()
	{
		std::vector<df::file_path> files;
		add_folder(test_files_folder, files);
		add_folder(test_formats_folder, files);

		const auto extra = environment_variable("DIFFRACTOR_XMP_CORPUS");

		for (const auto part : str::split(extra, true, [](const char c) { return c == ';'; }))
		{
			add_folder(df::folder_path(part), files);
		}

		return files;
	}

	struct labelled_packet
	{
		std::string label;
		std::string packet;
	};

	// Each packet libfiles' readers find where its writers put it, each one Diffractor's scanners find,
	// and every sidecar.
	std::vector<labelled_packet> corpus_packets()
	{
		std::vector<labelled_packet> packets;

		for (const auto& path : corpus_files())
		{
			const auto name = path.str();

			if (str::icmp(path.extension(), ".xmp"sv) == 0)
			{
				const auto bytes = df::blob_from_file(path);
				packets.push_back({name + " (sidecar)", std::string(bytes.begin(), bytes.end())});
				continue;
			}

			try
			{
				if (const auto f = open_file(path, platform::file_open_mode::read))
				{
					platform_file_stream stream(f);
					libfiles::reader r(stream);
					auto read = libfiles::read_packet(r);
					if (!read.packet.empty()) packets.push_back({name + " (read)", std::move(read.packet)});
				}
			}
			catch (...)
			{
			}

			files ff;
			const auto scanned = ff.scan_file(path, false, files::file_type_from_name(path));
			auto xmp = std::string(scanned.metadata.xmp.begin(), scanned.metadata.xmp.end());
			if (xmp.starts_with(xmp_signature)) xmp.erase(0, xmp_signature.size());
			if (!xmp.empty()) packets.push_back({name + " (scanned)", std::move(xmp)});
		}

		return packets;
	}

	// Packets that exercise what the fixtures may not: the repairs, aliases and malformed RDF.
	std::vector<labelled_packet> synthetic_packets()
	{
		const auto wrap = [](const std::string_view body)
		{
			return std::format("<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
			                   "<rdf:Description rdf:about='' xmlns:dc='http://purl.org/dc/elements/1.1/' "
			                   "xmlns:xmp='http://ns.adobe.com/xap/1.0/' xmlns:exif='http://ns.adobe.com/exif/1.0/' "
			                   "xmlns:tiff='http://ns.adobe.com/tiff/1.0/' xmlns:photoshop='http://ns.adobe.com/photoshop/1.0/' "
			                   "xmlns:xmpDM='http://ns.adobe.com/xmp/1.0/DynamicMedia/' "
			                   "xmlns:xmpRights='http://ns.adobe.com/xap/1.0/rights/'>{}</rdf:Description></rdf:RDF></x:xmpmeta>",
			                   body);
		};

		const std::string_view bodies[] = {
			"<dc:title>Simple title</dc:title><dc:subject>one</dc:subject><dc:creator>Ann</dc:creator><dc:rights/>",
			"<tiff:Artist>Ann</tiff:Artist><tiff:ImageDescription>Quay</tiff:ImageDescription><tiff:Copyright>C</tiff:Copyright>",
			"<exif:DateTimeDigitized>2020-01-02T03:04:05</exif:DateTimeDigitized><xmp:CreateDate>2021-01-01</xmp:CreateDate>",
			"<photoshop:Caption xml:lang='fr'>Quai</photoshop:Caption>",
			"<photoshop:Keywords><rdf:Bag><rdf:li>a</rdf:li></rdf:Bag></photoshop:Keywords><xmp:Title>T</xmp:Title>",
			"<dc:title><rdf:Alt><rdf:li xml:lang='de'>Kai</rdf:li><rdf:li xml:lang='x-default'>Quay</rdf:li></rdf:Alt></dc:title>",
			"<dc:description><rdf:Bag><rdf:li>no lang</rdf:li><rdf:li/><rdf:li xml:lang='EN-us'>yes</rdf:li></rdf:Bag></dc:description>",
			"<exif:UserComment>note</exif:UserComment><exif:GPSTimeStamp>T12:30:00Z</exif:GPSTimeStamp>"
			"<exif:DateTimeOriginal>2019-03-04T10:00:00.5+02:00</exif:DateTimeOriginal>",
			"<xmpDM:copyright>(c) Ann</xmpDM:copyright><dc:rights><rdf:Alt><rdf:li xml:lang='fr'>Droits</rdf:li></rdf:Alt></dc:rights>",
			"<dc:format rdf:parseType='Resource'><rdf:value xml:lang='en'>image/png</rdf:value><xmp:Label>q</xmp:Label></dc:format>",
			"<exif:Flash><rdf:Description exif:Fired='True'><exif:Mode>2</exif:Mode></rdf:Description></exif:Flash>",
			"<exif:Flash exif:Fired='True' exif:Mode='1' xml:lang='en'/><xmp:BaseURL rdf:resource='http://example.com/'/>",
			"<xmp:Label>caf\xE9\x01x &#x1F;y \x80 &#x0041; &#65;</xmp:Label>",
			"<xmp:Label>a &amp; b<![CDATA[ <c> ]]>\r\nd</xmp:Label><xmp:Rating>2</xmp:Rating><xmp:Rating>3</xmp:Rating>",
			"<dc:subject><rdf:Seq><rdf:li>b</rdf:li><rdf:_2>c</rdf:_2></rdf:Seq></dc:subject><dc:bad><rdf:li>x</rdf:li></dc:bad>",
			"<xmp:Thing rdf:parseType='Literal'>x</xmp:Thing><xmp:Other rdf:parseType='Collection'/><xmp:Rating>1</xmp:Rating>",
			"<xmp:Typed><xmp:SomeType xmp:a='1'/></xmp:Typed><xmp:Empty></xmp:Empty><xmp:Spaces>   </xmp:Spaces>",
			"<rdf:li>misplaced</rdf:li><xmp:Rating>4</xmp:Rating><about>x</about>",
		};

		std::vector<labelled_packet> packets;
		auto index = 0;

		for (const auto body : bodies)
		{
			packets.push_back({std::format("synthetic {}", ++index), wrap(body)});
		}

		packets.push_back({"synthetic uuid about", "<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		                   "<rdf:Description rdf:about='uuid:1234' xmlns:xmp='http://ns.adobe.com/xap/1.0/' xmp:Rating='1'/>"
		                   "<rdf:Description rdf:about='other' xmlns:xmp='http://ns.adobe.com/xap/1.0/' xmp:Label='L'/></rdf:RDF>"});
		packets.push_back({"synthetic two roots", "<doc><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		                   "<rdf:Description xmlns:xmp='http://ns.adobe.com/xap/1.0/' xmp:Rating='1'/></rdf:RDF>"
		                   "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		                   "<rdf:Description xmlns:xmp='http://ns.adobe.com/xap/1.0/' xmp:Rating='2'/></rdf:RDF></x:xmpmeta></doc>"});
		packets.push_back({"synthetic prefix clash", "<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		                   "<rdf:Description xmlns:dc='http://example.com/parity/' dc:thing='x'/></rdf:RDF>"});
		packets.push_back({"synthetic default namespace", "<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		                   "<rdf:Description xmlns='http://example.com/parity-default/'><thing>x</thing></rdf:Description></rdf:RDF>"});
		packets.push_back({"synthetic doctype", "<?xml version='1.0'?><!DOCTYPE x [<!ELEMENT x ANY>]>"
		                   "<rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
		                   "<rdf:Description xmlns:xmp='http://ns.adobe.com/xap/1.0/' xmp:Rating='1'/></rdf:RDF>"});
		packets.push_back({"synthetic not xml", "plain text"});
		packets.push_back({"synthetic empty", ""});

		return packets;
	}
}

static void should_parse_every_xmp_packet_whole_and_cut_short()
{
	auto packets = xmp_corpus::corpus_packets();
	const auto from_files = packets.size();

	for (auto& p : xmp_corpus::synthetic_packets()) packets.push_back(std::move(p));

	auto parsed = 0;
	auto refused = 0;
	std::vector<std::string> failures;

	for (const auto& p : packets)
	{
		// Whole, then cut short at many points: a malformed packet keeps what precedes its first fault.
		constexpr auto cuts = 48;

		for (auto i = cuts; i > 0; --i)
		{
			const auto at = p.packet.size() * i / cuts;
			const auto result = xmp_corpus::parse(std::string_view(p.packet).substr(0, at));

			if (!result.unexpected.empty())
			{
				failures.push_back(std::format("{} cut at {}: {}", p.label, at, result.unexpected));
			}
			else if (result.refused)
			{
				++refused;
			}
			else
			{
				++parsed;
			}
		}
	}

	df::log(__FUNCTION__, std::format("{} packets from {} files: {} parsed, {} refused, {} failures",
	                                  packets.size(), from_files, parsed, refused, failures.size()));

	std::string report;
	for (size_t i = 0; i < std::min<size_t>(failures.size(), 12); ++i) report += "\n  " + failures[i];

	assert_equal(true, from_files > 20, "the fixtures hold packets to parse");
	assert_equal(0, static_cast<int>(failures.size()), std::format("libfiles parses or refuses every packet{}", report));
}

// Each serialization libfiles writes reads back as the tree it was written from - in the same order,
// unless the form is compact - and an exact-length packet is exactly as long as asked. The fixtures'
// packets only: the synthetic ones exercise repairs, such as aliases and xmpDM:copyright, that a first
// reading leaves half done, as the toolkit's did, so what is written from them reads back repaired
// further.
static void should_read_back_every_xmp_packet_libfiles_writes()
{
	const auto packets = xmp_corpus::corpus_packets();

	struct form
	{
		const char* name;
		libfiles::xmp::serialize_options options;
		bool keeps_order;
	};

	constexpr uint32_t exact_length = 1 << 16;

	const form forms[] = {
		{"sidecar", {.omit_packet_wrapper = true}, true},
		{"packet", {}, true},
		{"compact", {.use_compact_format = true}, false},
		{"exact", {.use_compact_format = true, .exact_packet_length = true, .padding = exact_length}, false},
	};

	std::vector<std::string> differences;
	auto compared = 0;

	for (const auto& p : packets)
	{
		libfiles::xmp::meta meta;

		try
		{
			meta.parse(p.packet);
		}
		catch (...)
		{
			continue;
		}

		const auto written = xmp_corpus::dump(meta);

		for (const auto& f : forms)
		{
			std::string text;

			try
			{
				text = meta.serialize(f.options);
			}
			catch (const libfiles::xmp::error& e)
			{
				// A packet larger than the exact length cannot be written at it.
				if (!f.options.exact_packet_length)
				{
					differences.push_back(std::format("{} ({}): not written: {}", p.label, f.name, e.what()));
				}

				continue;
			}

			++compared;
			const auto reread = xmp_corpus::parse(text);

			if (reread.refused || !reread.unexpected.empty())
			{
				differences.push_back(std::format("{} ({}): libfiles refused what it wrote", p.label, f.name));
			}
			else if (f.keeps_order ? reread.dump != written
			                       : xmp_corpus::by_property(reread.dump) != xmp_corpus::by_property(written))
			{
				const auto expected = f.keeps_order ? written : xmp_corpus::by_property(written);
				const auto actual = f.keeps_order ? reread.dump : xmp_corpus::by_property(reread.dump);
				differences.push_back(std::format("{} ({}): {}", p.label, f.name, xmp_corpus::first_difference(expected, actual)));
			}
			else if (f.options.exact_packet_length && text.size() != exact_length)
			{
				differences.push_back(std::format("{} ({}): {} bytes, asked for {}", p.label, f.name, text.size(), exact_length));
			}
		}
	}

	std::string report;
	for (size_t i = 0; i < std::min<size_t>(differences.size(), 12); ++i) report += "\n  " + differences[i];

	assert_equal(true, compared > 80, "packets were written and reread");
	assert_equal(0, static_cast<int>(differences.size()), std::format("libfiles reads back what it writes{}", report));
}

void register_metadata_tests(view_state& state, test_registry& tests)
{
	tests.add("Should parse every xmp packet whole and cut short"s, should_parse_every_xmp_packet_whole_and_cut_short);
	tests.add("Should read back every xmp packet libfiles writes"s, should_read_back_every_xmp_packet_libfiles_writes);
	tests.add("Should collect descriptive fields"s, should_collect_descriptive_fields);
	tests.add("Should scan info from title"s, should_scan_info_from_title);
	tests.add("Should combine and deduplicate tag sets"s, should_combine_and_deduplicate_tag_sets);
	tests.add("Should copy preserve properties"s, should_copy_preserve_properties);
	tests.add("Should replace item metadata without resetting playback position"s,
	          should_replace_item_metadata_without_resetting_playback_position);
	tests.add("Should parse exif tags"s, should_parse_exif_tags);
	tests.add("Should read Windows XP tags as written"s, should_read_windows_xp_tags_as_written);
	tests.add("Should parse exif gps height"s, should_parse_exif_gps_height);
	tests.add("Should keep gps on the equator and meridian"s, should_keep_gps_on_the_equator_and_meridian);
	tests.add("Should parse an extended length iptc dataset"s, should_parse_an_extended_length_iptc_dataset);

	// Issue #65 - binary exif comment
	tests.add("Should drop binary exif comment"s, should_drop_binary_exif_comment);

	//
	// Tags
	//
	// Issue #209 - tag removal
	tests.add("Should remove tags"s, should_remove_tags);
	tests.add("Should remove tags case insensitive"s, should_remove_tags_case_insensitive);
	tests.add("Should remove multiple tags"s, should_remove_multiple_tags);
	tests.add("Should remove all tags"s, should_remove_all_tags);
	tests.add("Should remove nonexistent tag"s, should_remove_nonexistent_tag);

	// Issue #219 - Korean tags
	tests.add("Should parse Korean tags"s, should_parse_korean_tags);

	// Issue #184 - group by date created uses DateTimeOriginal
	tests.add("Should prefer DateTimeOriginal for created"s, should_prefer_datetimeoriginal_for_created);

	//
	// Dates - docs/metadata.md#dates
	//
	tests.add("Should hold date sources in authority order"s, should_hold_date_sources_in_authority_order);
	tests.add("Should collapse date sources that agree to the second"s,
	          should_collapse_sources_that_agree_to_the_second);
	tests.add("Should rank editor rewritten dates below the shutter"s,
	          should_rank_editor_rewritten_dates_below_the_shutter);
	tests.add("Should read exif DateTime as modified date"s, should_read_exif_datetime_as_modified);
	tests.add("Should take the modified date from the filesystem"s,
	          should_take_the_modified_date_from_the_filesystem);
	tests.add("Should not repeat a filesystem stamp in the date bubble"s,
	          should_not_repeat_a_filesystem_stamp_in_the_date_bubble);
	tests.add("Should carry the zone and fraction a date was written with"s,
	          should_carry_the_zone_and_fraction_a_date_was_written_with);
	tests.add("Should read a gps date stamp as a date"s, should_read_a_gps_date_stamp_as_a_date);
	tests.add("Should separate original from created date for a scan"s,
	          should_separate_original_from_created_for_a_scan);
	tests.add("Should fall back through created to modified date"s, should_fall_back_through_created_to_modified);
	tests.add("Should pack dates canonically regardless of parse order"s,
	          should_pack_dates_canonically_regardless_of_parse_order);
	// SRC-035 - preserve date representation when coalescing sources.
	tests.add("Should preserve date representation when coalescing sources"s,
	          should_preserve_date_representation_when_coalescing_sources);
	tests.add("Should record dates that do not fit"s, should_record_dates_that_do_not_fit);

	//
	// Scan Metadata
	//
	tests.add("Should scan jpg metadata"s, should_scan_jpeg);
	tests.add("Should scan TIFF camera metadata"s, should_scan_tiff_camera_metadata);
	tests.add("Should scan avi metadata"s, should_scan_avi);
	tests.add("Should scan mov metadata"s, should_scan_mov);
	tests.add("Should scan mkv metadata"s, [] { should_scan_matroska("tagged.mkv", "rawvideo"); });
	tests.add("Should scan webm metadata"s, [] { should_scan_matroska("tagged.webm", "vp8"); });
	tests.add("Should scan mp3 metadata"s, should_scan_mp3);
	tests.add("Should scan mp4 metadata"s, should_scan_mp4);
	// MOD-013 - scan-to-property conversion preserves supported high audio sample rates.
	tests.add("Should preserve high audio sample rates"s, should_preserve_high_audio_sample_rates);

	// Issue #3 - container tags added by an old version cannot be removed
	tests.add("Should not resurrect container tags"s, should_not_resurrect_container_tags);
	tests.add("Should apply property level Windows metadata precedence"s,
	          should_apply_property_level_windows_metadata_precedence);
	tests.add("Should scan raw metadata"s, should_scan_raw);
	// SRC-022 - RAW GPS preserves valid coordinates on a zero-valued axis.
	tests.add("Should scan mod metadata"s, should_scan_mod);
	tests.add("Should scan webp metadata"s, should_scan_webp);
	tests.add("Should scan jxl metadata"s, should_scan_jxl);
	tests.add("Should scan heif metadata"s, should_scan_heic);
	// SRC-018 - HEIF XMP is normally a MIME metadata item.
	tests.add("Should not double apply heif rotation"s, should_not_double_apply_heif_rotation);
	tests.add("Should scan avif metadata"s, should_scan_avif);
	// SRC-019 - PNG metadata may be stored after IDAT.
	tests.add("Should read png metadata after idat"s, should_read_png_metadata_after_idat);
	tests.add("Should reject incomplete PNG compressed metadata"s, should_reject_incomplete_png_compressed_metadata);
	tests.add("Should parse Xmp"s, should_parse_xmp);
	tests.add("Should apply Xmp GPS as a pair"s, should_apply_xmp_gps_as_a_pair);
	tests.add("Should read Xmp disc number"s, should_read_xmp_disc_number);
	tests.add("Should report empty Xmp packet as parsed"s, should_report_empty_xmp_packet_as_parsed);
	tests.add("Should clear tags from an empty Xmp subject"s, should_clear_tags_from_an_empty_xmp_subject);
	tests.add("Should read the declared panorama projection"s, should_read_the_declared_panorama_projection);
	tests.add("Should read embedded panorama geometry"s, should_read_embedded_panorama_geometry);
	tests.add("Should present exif metadata by ifd"s, should_present_exif_block_by_ifd);
	tests.add("Should write a complete orientation exif"s, should_write_a_complete_orientation_exif);
	tests.add("Should present maker note section"s, should_present_maker_note_section);
	tests.add("Should present derived exif section"s, should_present_derived_exif_section);
	tests.add("Should derive focal length facts"s, should_derive_focal_length_facts);
	tests.add("Should keep unknown Exif tags through a rotate"s, should_keep_unknown_exif_tags_through_a_rotate);
	tests.add("Should present exif tags as stored"s, should_present_exif_tags_as_stored);
	tests.add("Should present embedded thumbnail as an image"s, should_present_embedded_thumbnail_as_an_image);
	tests.add("Should present raw metadata sections"s, should_present_raw_block_sections);
	tests.add("Should present icc metadata sections"s, should_present_icc_block_sections);
	tests.add("Should read icc tags that share a region"s, should_read_icc_tags_that_share_a_region);
	tests.add("Should decode icc text exactly"s, should_decode_icc_text_exactly);
	tests.add("Should decode icc mluc offsets"s, should_decode_icc_mluc_offsets);
	tests.add("Should present jpeg structure"s, should_present_jpeg_structure_block);
	tests.add("Should present jpeg embedded images"s, should_present_jpeg_embedded_images);
	tests.add("Should present webp structure"s, should_present_webp_structure_block);
	tests.add("Should present heif structure"s, should_present_heif_structure_block);
	tests.add("Should merge Xmp sidecar"s, should_merge_xmp_sidecar);
	tests.add("Should replace tokens"s, should_replace_tokens);
}
