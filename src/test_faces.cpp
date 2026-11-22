// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Tests for faces (model_faces) - the face regions a picture's metadata names, turned upright and
// packed, and decoded pictures handed to libmug: over libfacedetection's sample photographs with Diffractor's
// tuning, in every orientation, from packed and planar decodes, reduced, over pictures with nobody in them and
// formats libmug does not read, and a face embedded alike from a turned decode. libmug's own tests hold its
// networks to libfacedetection's and OpenCV's. Then the face record and store, face terms, the face book with
// its database and CSV, finding people by confirmation, file names and resemblance, and reviewing faces.
// docs/faces.md owns the behaviour.

#include "pch.h"

#include "app_match.h"
#include "files.h"
#include "model_db.h"
#include "model_db_faces.h"
#include "model_faces.h"
#include "model_faces_search.h"
#include "test.h"
#include "test_fixtures.h"
#include "test_runner.h"
#include "ui_elements.h"
#include "view_faces.h"
#include "view_list.h"

#include "libdesktop/ui/test_support.h"

#include <sqlite3.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

// libfacedetection's own sample photographs - the ones its Android demonstration finds faces in - read
// from the archive the build fetched, so that this repository keeps no photograph of a person.
static core::file_path face_sample(const std::string_view name)
{
	return core::folder_path(DIFFRACTOR_FACEDETECTION_SOURCE_DIR)
	    .combine("mobile")
	    .combine("Android")
	    .combine("FaceDetection")
	    .combine("app")
	    .combine("src")
	    .combine("main")
	    .combine("assets")
	    .combine_file(name);
}

static core::surface_ptr decode_picture(const core::file_path path, const bool planar = false, const sizei bound = {})
{
	files ff;
	file_read_stream stream;
	if (!stream.open(path)) return {};
	core::blob owner;
	return ff.image_to_surface(stream.view_all(owner), bound, planar, decode_intent::display);
}

static double box_overlap(const rectd a, const rectd b)
{
	const auto width = std::min(a.right(), b.right()) - std::max(a.left(), b.left());
	const auto height = std::min(a.bottom(), b.bottom()) - std::max(a.top(), b.top());
	if (width <= 0.0 || height <= 0.0) return 0.0;
	const auto intersection = width * height;
	return intersection / (a.Width * a.Height + b.Width * b.Height - intersection);
}

static void assert_same_area(const rectd expected, const rectd actual, const std::string_view what,
                             const double tolerance = 1e-6)
{
	assert_near(expected.X, actual.X, tolerance, what, "left"sv);
	assert_near(expected.Y, actual.Y, tolerance, what, "top"sv);
	assert_near(expected.Width, actual.Width, tolerance, what, "width"sv);
	assert_near(expected.Height, actual.Height, tolerance, what, "height"sv);
}

static void should_turn_face_regions_upright_and_back()
{
	// A phone held upright: its sensor's first row is the picture's right-hand side, so an area near the
	// stored picture's top left is shown near its top right.
	const rectd stored{0.1, 0.2, 0.3, 0.1};
	assert_same_area({0.7, 0.1, 0.1, 0.3}, faces::to_upright(stored, core::orientation::right_top),
	                 "a quarter turn clockwise"sv);
	assert_same_area({0.2, 0.6, 0.1, 0.3}, faces::to_upright(stored, core::orientation::left_bottom),
	                 "a quarter turn anticlockwise"sv);
	assert_same_area({0.6, 0.7, 0.3, 0.1}, faces::to_upright(stored, core::orientation::bottom_right), "a half turn"sv);

	for (const auto orientation :
	     {core::orientation::top_left, core::orientation::top_right, core::orientation::bottom_right,
	      core::orientation::bottom_left, core::orientation::left_top, core::orientation::right_top,
	      core::orientation::right_bottom, core::orientation::left_bottom})
	{
		const auto turned = faces::to_upright(stored, orientation);
		assert_same_area(stored, faces::to_stored(turned, orientation), "turning back finds the stored area"sv);
	}
}

static void should_read_where_a_face_region_is_by_the_shape_it_was_measured_on()
{
	const std::vector<faces::stored_region> stored{{"Jane Doe", rectd{0.1, 0.2, 0.3, 0.1}}, {"Ann Lee", {}}};
	const sizei landscape{4000, 3000};

	// Measured on the picture as stored, as the guidelines say, or with no dimensions at all: turned as the
	// picture is shown.
	for (const auto applied_to : {sizei{4000, 3000}, sizei{2000, 1500}, sizei{}})
	{
		const auto upright = faces::upright_regions(stored, applied_to, landscape, core::orientation::right_top);
		assert_equal(2, static_cast<int>(upright.size()), "every region is kept"sv);
		assert_equal("Jane Doe"sv, std::string_view(upright[0].name), "its name"sv);
		assert_same_area({0.7, 0.1, 0.1, 0.3}, upright[0].area, "an area measured as stored is turned"sv);
		assert_equal(false, upright[1].has_area(), "a name with no area has none"sv);
	}

	// Measured on the picture as it is shown, a quarter turn from the stored shape: already upright.
	const auto shown = faces::upright_regions(stored, {3000, 4000}, landscape, core::orientation::right_top);
	assert_same_area({0.1, 0.2, 0.3, 0.1}, shown[0].area, "an area measured upright is kept as it is"sv);

	// Neither shape: the picture was cropped since, so the names stay and the areas go.
	const auto cropped = faces::upright_regions(stored, {4000, 2000}, landscape, core::orientation::right_top);
	assert_equal("Jane Doe"sv, std::string_view(cropped[0].name), "a cropped picture keeps its names"sv);
	assert_equal(false, cropped[0].has_area(), "but not areas that no longer fit it"sv);

	// A square picture cannot tell the two apart, and reads as stored.
	const auto square = faces::upright_regions(stored, {1000, 1000}, {1000, 1000}, core::orientation::right_top);
	assert_same_area({0.7, 0.1, 0.1, 0.3}, square[0].area, "a square picture's areas are turned"sv);
}

static void should_pack_face_regions_as_the_file_states_them()
{
	// As the file states them: on the picture as stored, measured on the dimensions it names.
	const std::vector<faces::stored_region> stated{{"Jane Doe", rectd{0.25, 0.5, 0.125, 0.25}},
	                                               {"", rectd{0.0, 0.0, 1.0, 1.0}},
	                                               {"Ann Lee", {}},
	                                               {"Past the edge", rectd{-0.1, 0.9, 0.3, 0.3}}};
	const sizei landscape{4000, 3000};
	const auto packed = faces::pack_regions(stated, landscape);
	constexpr auto tolerance = 1.0 / 16384.0;

	const auto unpacked = faces::unpack_regions(packed, landscape, core::orientation::top_left);
	assert_equal(4, static_cast<int>(unpacked.size()), "every region"sv);

	for (auto i = 0; i < 3; ++i)
	{
		assert_equal(stated[i].name, unpacked[i].name, "the name"sv);
		assert_equal(stated[i].area.has_value(), unpacked[i].has_area(), "whether it has an area"sv);
		if (stated[i].area) assert_same_area(*stated[i].area, unpacked[i].area, "the area"sv, tolerance);
	}

	assert_same_area({0.0, 0.9, 0.2, 0.1}, unpacked[3].area, "an area past the picture is read to its edge"sv,
	                 tolerance);

	// The judgment of a region's space is made as it is read, so the same packing read for a picture shown on its
	// side is turned with it, and one measured on a shape the picture no longer has keeps its names alone.
	const auto turned = faces::unpack_regions(packed, landscape, core::orientation::right_top);
	assert_same_area(faces::to_upright({0.25, 0.5, 0.125, 0.25}, core::orientation::right_top), turned[0].area,
	                 "turned as the picture is shown"sv, tolerance);

	const auto stale = faces::unpack_regions(faces::pack_regions(stated, {4000, 2000}), landscape,
	                                         core::orientation::right_top);
	assert_equal("Jane Doe"sv, std::string_view(stale.at(0).name), "a cropped picture keeps its names"sv);
	assert_equal(false, stale.at(0).has_area(), "but not areas that no longer fit it"sv);

	// A packing from before regions were kept as stated held them upright already, and reads as it is.
	std::string upright{"\x01\x04Jane"};
	for (const uint16_t v : {uint16_t{0x4000}, uint16_t{0x8000}, uint16_t{0x2000}, uint16_t{0x4000}})
	{
		upright.push_back(static_cast<char>(v & 0xff));
		upright.push_back(static_cast<char>(v >> 8));
	}
	const auto earlier = faces::unpack_regions(core::cache(upright), landscape, core::orientation::right_top);
	assert_equal(1, static_cast<int>(earlier.size()), "an earlier packing reads"sv);
	assert_same_area({0.25, 0.5, 0.125, 0.25}, earlier.at(0).area, "as the upright area it held"sv, 1.0 / 16384.0);

	// A later release's packing - a longer header, and more to each region - is read for what this build knows.
	const auto stated_bytes = std::string(packed.sv());
	std::string later = stated_bytes.substr(0, 11);
	later[1] = static_cast<char>(11 + 4);
	later[2] = static_cast<char>(8 + 2);
	later.append(4, '\xEE');
	for (size_t pos = 11; pos < stated_bytes.size();)
	{
		const auto region_len = 1 + static_cast<size_t>(static_cast<uint8_t>(stated_bytes[pos])) + 8;
		later += stated_bytes.substr(pos, region_len);
		later.append(2, '\xEE');
		pos += region_len;
	}
	const auto stepped = faces::unpack_regions(core::cache(later), landscape, core::orientation::top_left);
	assert_equal(4, static_cast<int>(stepped.size()), "a later packing reads"sv);
	assert_same_area(*stated[0].area, stepped.at(0).area, "its fields stepped over"sv, tolerance);
	assert_equal(stated[3].name, stepped.at(3).name, "to the last region"sv);

	// A name too long for its byte of length is cut at a character, not inside one.
	std::string long_name(254, 'a');
	long_name += "\xC3\xA9\xC3\xA9";
	const auto cut = faces::unpack_regions(faces::pack_regions(std::vector<faces::stored_region>{{long_name, {}}}, {}),
	                                       {}, core::orientation::top_left);
	assert_equal(254, static_cast<int>(cut.at(0).name.size()), "a long name is cut before a split character"sv);

	assert_equal(true, faces::pack_regions({}, landscape).is_empty(), "no regions pack to nothing"sv);
	assert_equal(0,
	             static_cast<int>(
					 faces::unpack_regions(core::cache("\x09garbage"sv), landscape, core::orientation::top_left)
						 .size()),
	             "a packing this build does not read unpacks to none"sv);
	assert_equal(0,
	             static_cast<int>(
					 faces::unpack_regions(core::cache("\x01\x05Jane"sv), landscape, core::orientation::top_left)
						 .size()),
	             "nor does one cut short"sv);
	assert_equal(0,
	             static_cast<int>(faces::unpack_regions(core::cache(std::string_view(packed.sv()).substr(0, 12)),
	                                                    landscape, core::orientation::top_left)
	                                  .size()),
	             "however it was packed"sv);
}

static void should_read_the_people_a_pictures_metadata_names()
{
	const std::string packet =
		R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
		R"(<rdf:Description rdf:about="" xmlns:mwg-rs="http://www.metadataworkinggroup.com/schemas/regions/")"
		R"( xmlns:stArea="http://ns.adobe.com/xmp/sType/Area#" xmlns:stDim="http://ns.adobe.com/xap/1.0/sType/Dimensions#")"
		R"( xmlns:Iptc4xmpExt="http://iptc.org/std/Iptc4xmpExt/2008-02-29/">)"
		R"(<mwg-rs:Regions rdf:parseType="Resource">)"
		R"(<mwg-rs:AppliedToDimensions stDim:w="4000" stDim:h="3000" stDim:unit="pixel"/>)"
		R"(<mwg-rs:RegionList><rdf:Bag>)"
		R"(<rdf:li rdf:parseType="Resource"><mwg-rs:Name>Jane Doe</mwg-rs:Name><mwg-rs:Type>Face</mwg-rs:Type>)"
		R"(<mwg-rs:Area stArea:x="0.25" stArea:y="0.25" stArea:w="0.3" stArea:h="0.1" stArea:unit="normalized"/>)"
		R"(</rdf:li></rdf:Bag></mwg-rs:RegionList></mwg-rs:Regions>)"
		R"(<Iptc4xmpExt:PersonInImage><rdf:Bag><rdf:li>Ann Lee</rdf:li></rdf:Bag></Iptc4xmpExt:PersonInImage>)"
		R"(</rdf:Description></rdf:RDF></x:xmpmeta>)";

	// A phone's portrait: stored on its side, at the dimensions the regions were measured on.
	file_scan_result scanned;
	scanned.width = 4000;
	scanned.height = 3000;
	scanned.orientation = core::orientation::right_top;
	scanned.metadata.xmp.assign(packet.begin(), packet.end());

	const auto md = scanned.to_props();
	const auto people = core::split(md->people, true);
	assert_equal(2, static_cast<int>(people.size()), "both people are named"sv);
	assert_equal("Jane Doe"sv, people[0], "the region's name"sv);
	assert_equal("Ann Lee"sv, people[1], "and PersonInImage's"sv);

	const auto regions = faces::unpack_regions(md->face_regions, md->dimensions(), md->orientation);
	assert_equal(1, static_cast<int>(regions.size()), "one face region"sv);
	assert_same_area({0.7, 0.1, 0.1, 0.3}, regions[0].area,
	                 "turned upright as it is read, against the picture's shape and orientation"sv, 1.0 / 16384.0);
}

static void assert_same_faces(const faces::face_set& expected, const faces::face_set& actual, const double tolerance,
                              const std::string_view what)
{
	assert_equal(static_cast<int>(expected.faces.size()), static_cast<int>(actual.faces.size()), what,
	             "the same faces are found"sv);

	for (auto i = 0_z; i < std::min(expected.faces.size(), actual.faces.size()); ++i)
	{
		const auto& e = expected.faces[i];
		const auto& a = actual.faces[i];
		assert_near(e.bounds.X, a.bounds.X, tolerance, what, "box left"sv);
		assert_near(e.bounds.Y, a.bounds.Y, tolerance, what, "box top"sv);
		assert_near(e.bounds.Width, a.bounds.Width, tolerance, what, "box width"sv);
		assert_near(e.bounds.Height, a.bounds.Height, tolerance, what, "box height"sv);

		for (auto m = 0; m < faces::landmark_count; ++m)
		{
			assert_near(e.marks[m].X, a.marks[m].X, tolerance, what, "landmark x"sv);
			assert_near(e.marks[m].Y, a.marks[m].Y, tolerance, what, "landmark y"sv);
		}
	}
}

static void should_detect_the_face_in_a_portrait()
{
	const auto portrait = decode_picture(face_sample("test.jpg"));
	assert_equal(true, core::is_valid(portrait), "libfacedetection's portrait decodes"sv);

	libmug::face_detector detector;
	const auto found = faces::find_faces(detector, portrait);
	assert_equal(240, found.extent.cx, "faces are measured in the picture's own dimensions"sv);
	assert_equal(320, found.extent.cy, "faces are measured in the picture's own dimensions"sv);
	assert_equal(1, static_cast<int>(found.faces.size()), "one face in a portrait"sv);

	const auto& face = found.faces.front();
	assert_equal(true, face.confidence >= 85, "a clear face is found with confidence"sv);

	// The landmarks are a face's, in the detector's order: the eyes either side of the nose and above
	// it, the nose above the mouth.
	const auto& [left_eye, right_eye, nose, left_mouth, right_mouth] = face.marks;
	assert_equal(true, left_eye.X < nose.X && nose.X < right_eye.X, "the nose sits between the eyes"sv);
	assert_equal(true, std::max(left_eye.Y, right_eye.Y) < nose.Y, "the eyes are above the nose"sv);
	assert_equal(true, nose.Y < std::min(left_mouth.Y, right_mouth.Y), "the nose is above the mouth"sv);
	assert_equal(true, left_mouth.X < right_mouth.X, "the mouth corners are in order"sv);
	assert_equal(true, std::ranges::all_of(face.marks, [&face](const pointd p) { return face.bounds.contains(p); }),
	             "the landmarks lie inside the box"sv);

	// Both floors decide what is reported.
	faces::tuning sure;
	sure.detector_confidence = face.confidence + 1;
	assert_equal(0, static_cast<int>(faces::find_faces(detector, portrait, sure).faces.size()),
	             "a face below the confidence floor is not reported"sv);

	faces::tuning large;
	large.min_face_edge_fraction = face.bounds.Width / found.extent.cx + 0.05;
	assert_equal(0, static_cast<int>(faces::find_faces(detector, portrait, large).faces.size()),
	             "a face below the size floor is not reported"sv);
}

// Most formats decode in the file's own orientation, so a portrait photograph reaches the detector on
// its side. The faces it reports are those of the picture as it is shown, whichever way it was stored.
static void should_detect_faces_whatever_the_pictures_orientation()
{
	const auto upright = decode_picture(face_sample("test.jpg"));
	assert_equal(true, core::is_valid(upright), "libfacedetection's portrait decodes"sv);

	libmug::face_detector detector;
	const auto expected = faces::find_faces(detector, upright);
	assert_equal(1, static_cast<int>(expected.faces.size()), "the upright portrait's face"sv);

	for (auto o = static_cast<int>(core::orientation::top_right); o <= static_cast<int>(core::orientation::left_bottom);
	     ++o)
	{
		const auto orientation = static_cast<core::orientation>(o);

		// Stored so that the application's own turn shows the upright photograph.
		const auto stored = upright->transform(core::to_simple_transform_inv(orientation));
		assert_equal(true, core::is_valid(stored), "the photograph is stored turned"sv);
		stored->orientation(orientation);
		assert_equal(core::flips_xy(orientation), stored->dimensions().cx == upright->dimensions().cy,
		             "a quarter turn is stored on its side"sv);

		const auto found = faces::find_faces(detector, stored);
		assert_equal(upright->dimensions().cx, found.extent.cx, "faces are measured upright"sv);
		assert_equal(upright->dimensions().cy, found.extent.cy, "faces are measured upright"sv);

		// Nothing is resampled at this size, so the network reads the same plane and answers the same.
		assert_same_faces(expected, found, 1e-3, "the faces of the picture as it is shown"sv);
	}
}

static void should_detect_faces_alike_in_planar_and_packed_decodes()
{
	const auto packed = decode_picture(face_sample("test2.jpg"));
	const auto planar = decode_picture(face_sample("test2.jpg"), true);
	assert_equal(true, core::is_valid(packed) && core::is_packed(packed->format()), "the sample decodes packed"sv);
	assert_equal(true, core::is_valid(planar) && planar->format() == core::texture_format::NV12,
	             "the sample decodes planar"sv);

	libmug::face_detector detector;
	const auto from_packed = faces::find_faces(detector, packed);
	const auto from_planar = faces::find_faces(detector, planar);

	// The planar decode's luminance comes straight from its Y plane and the packed one's from colour
	// converted back, so they differ by rounding and no more.
	assert_same_faces(from_packed, from_planar, 0.5, "one picture, decoded either way"sv);
}

static void should_detect_faces_in_a_picture_larger_than_the_detector_reads()
{
	const auto group = decode_picture(face_sample("test2.jpg"));
	assert_equal(true, core::is_valid(group), "libfacedetection's group photograph decodes"sv);

	libmug::face_detector detector;
	const auto full = faces::find_faces(detector, group);

	// The detector reads a plane whose long edge is 320 here, so every box it finds is scaled back to
	// the picture's own dimensions.
	faces::tuning bounded;
	bounded.detector_edge = 320;
	const auto reduced = faces::find_faces(detector, group, bounded);
	assert_equal(group->dimensions().cx, reduced.extent.cx, "faces are measured in the picture's dimensions"sv);
	assert_equal(static_cast<int>(full.faces.size()), static_cast<int>(reduced.faces.size()),
	             "a reduced picture still shows these faces"sv);

	for (const auto& face : reduced.faces)
	{
		// On a plane little more than half the picture's width a face is some twenty pixels across,
		// and its box moves by a pixel or two of them.
		assert_equal(true,
		             std::ranges::any_of(full.faces, [&face](const faces::face& f)
		{ return box_overlap(face.bounds, f.bounds) >= 0.6; }),
		             "each face is where the full picture has it"sv);
	}

	// Reduced while still on its side and turned after, a stored quarter turn reads as nearly the same
	// plane as the upright picture.
	const auto stored = group->transform(core::to_simple_transform_inv(core::orientation::right_top));
	assert_equal(true, core::is_valid(stored), "the photograph is stored turned"sv);
	stored->orientation(core::orientation::right_top);
	assert_same_faces(reduced, faces::find_faces(detector, stored, bounded), 1.5,
	                  "a reduced picture stored on its side"sv);
}

static void should_detect_no_face_where_there_is_none()
{
	libmug::face_detector detector;
	libmug::face_embedder embedder;

	// A city's windows and a dog looking at the camera must not acquire faces on a closer look.
	for (const auto* const name : {"IMG_9340.JPG", "IMG_0096.JPG"})
	{
		const auto picture = decode_picture(test_files_folder().combine_file(name), false,
		                                    {faces::defaults().decode_edge, faces::defaults().decode_edge});
		assert_equal(true, core::is_valid(picture), name, "the fixture decodes"sv);
		assert_equal(0_z, faces::read_record(detector, embedder, picture, {}).faces.size(), name,
		             "nobody is in the picture"sv);
	}

	const auto ten_bit = std::make_shared<core::surface>();
	assert_equal(true, ten_bit->alloc({64, 64}, core::texture_format::P010) != nullptr, "P010 surface"sv);
	assert_equal(0, static_cast<int>(faces::find_faces(detector, ten_bit).faces.size()),
	             "a format the detector does not read finds no face"sv);
	assert_equal(0, static_cast<int>(faces::find_faces(detector, nullptr).faces.size()), "no picture, no face"sv);
}

// A picture whose colour changes everywhere, so a crop sampled from the wrong pixels would show: three waves of
// whole numbers.
static core::surface_ptr wave_picture(const bool turned)
{
	const auto wave = [](const int t)
	{
		return static_cast<uint8_t>(std::abs(t % 510 - 255));
	};

	// Turned, the picture is stored on its side - its first row is its right-hand side - as a phone holding it
	// upright stores it, and says so by its orientation.
	const sizei shown{400, 300};
	const auto stored = turned ? sizei{shown.cy, shown.cx} : shown;
	auto result = std::make_shared<core::surface>();
	if (!result->alloc(stored, core::texture_format::ARGB)) return {};
	result->orientation(turned ? core::orientation::right_top : core::orientation::top_left);

	for (auto y = 0; y < shown.cy; ++y)
	{
		for (auto x = 0; x < shown.cx; ++x)
		{
			const auto sx = turned ? y : x;
			const auto sy = turned ? shown.cx - 1 - x : y;
			auto* const p = result->pixels_line(sy) + static_cast<size_t>(sx) * 4;
			p[0] = wave(x * 5 + y * 3);
			p[1] = wave(x * 2 + y * 7);
			p[2] = wave(x * 3 + y + 100);
			p[3] = 255;
		}
	}

	return result;
}

static const faces::landmarks wave_face{pointd{151.25, 121.5}, pointd{229.75, 117.25}, pointd{192.5, 161.75},
                                        pointd{158.0, 201.5}, pointd{223.5, 197.0}};

// The picture's face, embedded as the scan embeds one, from its upright landmarks.
static std::optional<faces::face_vector> embed_wave_face(libmug::face_embedder& embedder, const bool turned)
{
	const auto picture = wave_picture(turned);
	faces::face_vector result{};
	if (!picture || !faces::embed_face(embedder, picture, wave_face, result)) return {};
	return result;
}

static void should_embed_a_face_alike_whatever_the_decodes_orientation()
{
	libmug::face_embedder embedder;
	assert_equal(true, embedder.ready(), "the network loads"sv);

	// A quarter turn moves every pixel centre onto another, so the crop - and the vector - are the same.
	const auto upright = embed_wave_face(embedder, false);
	const auto turned = embed_wave_face(embedder, true);
	assert_equal(true, upright.has_value() && turned.has_value(), "both embed"sv);
	assert_equal(true, faces::similarity(*upright, *turned) > 0.99999f, "a stored turn changes nothing"sv);
}

static void should_quantize_face_hashes_closely()
{
	// Vectors of the kind SFace makes: normalised, their values spread around zero.
	std::mt19937 random(7);
	std::normal_distribution<float> spread(0.0f, 1.0f);
	const auto make = [&]
	{
		faces::face_vector v{};
		auto length = 0.0f;
		for (auto& x : v)
		{
			x = spread(random);
			length += x * x;
		}
		for (auto& x : v) x /= std::sqrt(length);
		return v;
	};

	auto worst = 0.0f;
	auto total = 0.0f;
	constexpr auto pairs = 200;

	for (auto i = 0; i < pairs; ++i)
	{
		const auto a = make();
		auto b = make();
		// Half the pairs resemble each other, as faces of one person do.
		if (i % 2 == 0)
		{
			auto length = 0.0f;
			for (auto k = 0; k < faces::vector_size; ++k)
			{
				b[k] = a[k] + b[k] * 0.8f;
				length += b[k] * b[k];
			}
			for (auto& x : b) x /= std::sqrt(length);
		}

		const auto error = std::abs(faces::similarity(a, b) -
		                            faces::similarity(faces::quantize(a), faces::quantize(b)));
		worst = std::max(worst, error);
		total += error;
	}

	// docs/faces.md#51-the-face-record: typically less than a thousandth.
	assert_equal(true, total / pairs < 0.001f, std::format("mean {}", total / pairs),
	             "a face hash keeps its similarities"sv);
	assert_equal(true, worst < 0.005f, std::format("worst {}", worst), "even at its worst"sv);
	assert_equal(true, faces::quantize(faces::face_vector{}).is_empty(), "nothing quantizes to nothing"sv);

	// The dot product written for the processor is the plain one, to the last unit, at the extremes too.
	std::uniform_int_distribution<int> byte(-127, 127);
	for (auto i = 0; i < 50; ++i)
	{
		std::array<int8_t, faces::vector_size> a{};
		std::array<int8_t, faces::vector_size> b{};
		for (auto k = 0; k < faces::vector_size; ++k)
		{
			a[k] = static_cast<int8_t>(i == 0 ? 127 : i == 1 ? -127 : byte(random));
			b[k] = static_cast<int8_t>(i == 0 ? -127 : i == 1 ? -127 : byte(random));
		}
		int32_t plain = 0;
		for (auto k = 0; k < faces::vector_size; ++k) plain += a[k] * b[k];
		assert_equal(plain, faces::dot_values(a.data(), b.data()), "the dot product"sv);
	}
}

static void should_measure_how_square_on_a_face_is()
{
	const faces::landmarks square_on{pointd{100, 100}, pointd{160, 100}, pointd{130, 130}, pointd{110, 160},
	                                 pointd{150, 160}};
	const auto q = faces::measure_quality(square_on);
	assert_near(60.0, q.eye_distance, 1e-9, "the eyes' distance"sv);
	assert_near(1.0, q.frontal, 1e-9, "a nose between level eyes is square-on"sv);
	assert_equal(true, faces::is_good(q, 95), "a large square-on face is good"sv);
	assert_equal(false, faces::is_good(q, 70), "unless the detector doubts it"sv);

	const faces::landmarks turned{pointd{100, 100}, pointd{160, 100}, pointd{158, 130}, pointd{110, 160},
	                              pointd{150, 160}};
	assert_equal(true, faces::measure_quality(turned).frontal < 0.2, "a nose over one eye is a face turned away"sv);

	const faces::landmarks small{pointd{100, 100}, pointd{110, 100}, pointd{105, 105}, pointd{102, 110},
	                             pointd{108, 110}};
	assert_equal(false, faces::is_good(faces::measure_quality(small), 95), "a small face carries too little"sv);
}

// A face near the picture's edge, or larger than its shorter side allows a margin for, is cut from inside the picture
// - the square moved in, or made no wider than that side - rather than beside a smear of its edge pixels.
static void should_keep_a_face_tile_inside_its_picture()
{
	// Each pixel's red is its column and its green its row, so a tile shows where in the picture it was cut from.
	auto picture = std::make_shared<core::surface>();
	picture->alloc(200, 100, core::texture_format::RGB);
	for (auto y = 0; y < 100; ++y)
	{
		auto* line = picture->pixels_line(y);
		for (auto x = 0; x < 200; ++x)
		{
			line[x * 4] = static_cast<uint8_t>(x);
			line[x * 4 + 1] = static_cast<uint8_t>(y);
			line[x * 4 + 2] = 0;
			line[x * 4 + 3] = 255;
		}
	}

	const auto at_edge = faces::cut_tile(picture, rectd{0.0, 0.3, 0.15, 0.3}, 48);
	assert_equal(true, at_edge != nullptr, "a face at the left edge is cut"sv);
	if (!at_edge) return;
	const auto* row = at_edge->pixels_line(24);
	assert_equal(true, row[0] == 0 && row[4] > row[0] && row[40] > row[36],
	             std::format("from the picture's own columns, from its edge in ({}, {}, {})", row[0], row[4], row[40]));

	const auto tall = faces::cut_tile(picture, rectd{0.4, 0.0, 0.4, 1.0}, 50);
	assert_equal(true, tall != nullptr, "a face as tall as the picture is cut"sv);
	if (!tall) return;
	assert_equal(true, tall->pixels_line(0)[1] <= 1 && tall->pixels_line(1)[1] > tall->pixels_line(0)[1],
	             "no wider than the picture's height, from its top row down"sv);
}

// A vector pointing mostly along one axis, so faces made from one axis resemble each other and no other.
static faces::face_vector axis_vector(const int axis, const float spread, const int seed)
{
	faces::face_vector v{};
	std::mt19937 random(static_cast<uint32_t>(seed));
	std::normal_distribution<float> noise(0.0f, spread);
	for (auto& x : v) x = noise(random);
	v[axis] += 1.0f;
	auto length = 0.0f;
	for (const auto x : v) length += x * x;
	for (auto& x : v) x /= std::sqrt(length);
	return v;
}

static faces::found_face face_like(const faces::face_vector& v, const rectd box, const bool good = true)
{
	faces::found_face f;
	f.box = box;
	f.confidence = 95;
	f.frontal = 230;
	f.eye_distance = good ? 60 : 10;
	f.flags = good ? faces::face_flag::good : 0;
	f.hash = faces::quantize(v);
	return f;
}

static void should_pack_a_face_record_as_the_index_keeps_it()
{
	faces::face_record record;
	record.model = faces::model_id();
	record.stamp = core::date_t(2024, 5, 6, 7, 8, 9);
	record.extent = {1600, 1200};
	record.faces.push_back(face_like(axis_vector(3, 0.05f, 1), {0.25, 0.5, 0.125, 0.25}));
	record.faces.push_back(face_like(axis_vector(9, 0.05f, 2), {-0.1, 0.9, 0.3, 0.3}, false));
	record.faces[0].marks[2] = {0.3, 0.6};
	record.faces[0].dismissed = {"Jane Doe", "Ann"};
	record.faces[1].flags |= faces::face_flag::hidden;

	const auto packed = faces::pack_record(record);
	assert_equal(true, packed.size() < 24 + 2 * 170 + 40, "about 170 bytes a face"sv);

	const auto unpacked = faces::unpack_record(packed);
	assert_equal(true, unpacked.has_value(), "the record reads back"sv);
	assert_equal(record.model, unpacked->model, "its model"sv);
	assert_equal(record.detection_revision, unpacked->detection_revision, "its detection revision"sv);
	assert_equal(true, record.stamp == unpacked->stamp, "its stamp"sv);
	assert_equal(1600, unpacked->extent.cx, "its extent"sv);
	assert_equal(2, static_cast<int>(unpacked->faces.size()), "its faces"sv);
	assert_same_area(record.faces[1].box, unpacked->faces[1].box, "a box past the edge, to 16 bits"sv, 1e-4);
	assert_near(0.3, unpacked->faces[0].marks[2].X, 1e-4, "a landmark"sv);
	assert_equal(true, record.faces[0].hash == unpacked->faces[0].hash, "the face hash exactly"sv);
	assert_equal(true, unpacked->faces[1].is_hidden() && !unpacked->faces[1].is_good(), "the flags"sv);
	assert_equal(2, static_cast<int>(unpacked->faces[0].dismissed.size()), "the dismissals"sv);
	assert_equal("Jane Doe"sv, std::string_view(unpacked->faces[0].dismissed[0]), "a dismissed name"sv);

	// Goodness is judged as a record is read, from the measures it keeps, so floors corrected in a later release need
	// no picture read again: a flag set by an earlier judgment does not stand against them.
	auto judged = record;
	judged.faces[0].flags &= static_cast<uint8_t>(~faces::face_flag::good);
	judged.faces[1].flags |= faces::face_flag::good;
	const auto rejudged = faces::unpack_record(faces::pack_record(judged));
	assert_equal(true, rejudged && rejudged->faces[0].is_good() && !rejudged->faces[1].is_good(),
	             "a face is good by its measures, whatever flag it was kept with"sv);

	// A later release's record: a longer header and a longer face, which this build steps over.
	constexpr size_t header = 24;
	const size_t stride = packed[2];
	std::vector<uint8_t> later(packed.begin(), packed.begin() + header);
	later[1] = header + 4;
	later[2] = static_cast<uint8_t>(stride + 2);
	later.insert(later.end(), 4, uint8_t{0xEE});
	for (size_t i = 0; i < 2; ++i)
	{
		const auto start = packed.begin() + static_cast<ptrdiff_t>(header + i * stride);
		later.insert(later.end(), start, start + static_cast<ptrdiff_t>(stride));
		later.insert(later.end(), 2, uint8_t{0xEE});
	}
	later.insert(later.end(), packed.begin() + static_cast<ptrdiff_t>(header + 2 * stride), packed.end());

	const auto stepped = faces::unpack_record(later);
	assert_equal(true, stepped.has_value(), "a later record reads"sv);
	assert_equal(true, record.faces[1].hash == stepped->faces[1].hash, "its fields stepped over"sv);
	assert_equal(2, static_cast<int>(stepped->faces[0].dismissed.size()), "and its dismissals still found"sv);

	// docs/faces.md#33-videos: a video's record says so, and keeps each face's frame and where its person was seen.
	auto video = record;
	video.video = true;
	video.faces[0].time_ms = 61'250;
	video.faces[0].seen = {{0, 4'500}, {58'000, 70'125}};
	video.faces[1].time_ms = 3'000;
	video.faces[1].seen = {{2'500, 3'500}};
	const auto video_packed = faces::pack_record(video);
	const auto video_read = faces::unpack_record(video_packed);
	auto expected = *faces::unpack_record(packed);
	expected.video = true;
	for (size_t i = 0; i < expected.faces.size(); ++i)
	{
		expected.faces[i].time_ms = video.faces[i].time_ms;
		expected.faces[i].seen = video.faces[i].seen;
	}
	assert_equal(true, video_read && *video_read == expected, "a video's record reads back whole"sv);
	assert_equal(false, faces::unpack_record(packed)->video, "and a picture's is not a video's"sv);
	assert_equal(false, faces::unpack_record(std::span(video_packed).first(video_packed.size() - 3)).has_value(),
	             "a cut list of where faces were seen reads as none"sv);

	// One written before faces kept a time - a shorter face, and no flags - reads as a picture's, at no time.
	constexpr size_t earlier_stride = 166;
	std::vector<uint8_t> earlier(packed.begin(), packed.begin() + header);
	earlier[2] = earlier_stride;
	earlier[22] = earlier[23] = 0;
	for (size_t i = 0; i < 2; ++i)
	{
		const auto start = packed.begin() + static_cast<ptrdiff_t>(header + i * stride);
		earlier.insert(earlier.end(), start, start + static_cast<ptrdiff_t>(earlier_stride));
	}
	earlier.insert(earlier.end(), packed.begin() + static_cast<ptrdiff_t>(header + 2 * stride), packed.end());
	const auto read_earlier = faces::unpack_record(earlier);
	assert_equal(true, read_earlier && *read_earlier == *faces::unpack_record(packed),
	             "a record from before faces kept a time reads as it did"sv);

	auto old = packed;
	old[21] = 0; // The formerly reserved detection byte.
	const auto before_retry = faces::unpack_record(old);
	assert_equal(true,
	             before_retry && before_retry->detection_revision < faces::face_detection_revision &&
	                 before_retry->faces[0].hash == record.faces[0].hash,
	             "an older scan can be revisited without invalidating its signatures");

	assert_equal(false, faces::unpack_record(std::span(packed).first(30)).has_value(), "a cut record reads as none"sv);
	auto future = packed;
	future[0] = 9;
	assert_equal(false, faces::unpack_record(future).has_value(), "nor does a format this build does not read"sv);
}

// A correction to how faces are found has the pictures read again, and the marks the user put on their faces carry to
// the faces found again in the same places, rather than going with the record they were kept in.
static void should_keep_the_marks_on_faces_found_again()
{
	faces::face_record before;
	before.faces.push_back(face_like(axis_vector(3, 0.05f, 1), {0.1, 0.1, 0.2, 0.2}));
	before.faces.push_back(face_like(axis_vector(9, 0.05f, 2), {0.6, 0.5, 0.2, 0.2}));
	before.faces[0].flags |= faces::face_flag::hidden;
	before.faces[1].dismissed = {"Jane Doe"};

	// Found again a little apart, in another order, beside a face the earlier reading missed.
	faces::face_record after;
	after.faces.push_back(face_like(axis_vector(9, 0.05f, 3), {0.61, 0.51, 0.19, 0.2}));
	after.faces.push_back(face_like(axis_vector(5, 0.05f, 4), {0.35, 0.7, 0.1, 0.1}));
	after.faces.push_back(face_like(axis_vector(3, 0.05f, 5), {0.11, 0.09, 0.2, 0.21}));
	after.faces[0].dismissed = {"jane doe"};

	faces::carry_marks(before, after);
	assert_equal(true, after.faces[2].is_hidden(), "a hidden face stays hidden"sv);
	assert_equal(false, after.faces[0].is_hidden() || after.faces[1].is_hidden(), "and no other is hidden"sv);
	assert_equal(1, static_cast<int>(after.faces[0].dismissed.size()), "a dismissal carried once, whatever its case"sv);
	assert_equal(true, after.faces[1].dismissed.empty(), "and a face found for the first time has no marks"sv);
}

// docs/faces.md#33-videos: a video is read at the first key frame of each of 256 parts of its length - a short clip at
// every key frame, a long one at no more than 256 - whatever its key frames are.
static void should_read_a_video_at_the_first_key_frame_of_each_part()
{
	const auto reads = [](const double length, const std::vector<double>& keys)
	{
		std::vector<double> read;
		faces::read_video_slices(length, [&](const double from) -> std::optional<double>
		{
			const auto next = std::ranges::lower_bound(keys, from - 0.001);
			if (next == keys.end()) return std::nullopt;
			read.push_back(*next);
			return *next;
		});
		return read;
	};
	const auto every = [](const double length, const double gap)
	{
		std::vector<double> keys;
		for (auto i = 0; i * gap < length; ++i) keys.push_back(i * gap);
		return keys;
	};

	const auto phone = every(60.0, 1.0);
	assert_equal(true, reads(60.0, phone) == phone, "a phone's clip, a key frame a second, is read at every one"sv);

	const auto intra = reads(120.0, every(120.0, 1.0 / 30.0));
	assert_equal(faces::video_slices, static_cast<int>(intra.size()), "a clip all of key frames is read 256 times"sv);
	auto even = intra.front() < 0.05 && intra.back() > 119.0;
	for (size_t i = 1; i < intra.size(); ++i) even = even && intra[i] - intra[i - 1] < 120.0 / faces::video_slices * 2;
	assert_equal(true, even, "evenly, from its start to its end"sv);

	assert_equal(faces::video_slices, static_cast<int>(reads(6000.0, every(6000.0, 3.4)).size()),
	             "a film with key frames seconds apart is read no more than 256 times"sv);
	assert_equal(1, static_cast<int>(reads(5.0, {0.0}).size()), "a clip with one key frame is read once"sv);
	assert_equal(10, static_cast<int>(reads(0.0, every(10.0, 1.0)).size()),
	             "one whose length is not known is read a key frame at a time"sv);

	auto calls = 0;
	faces::read_video_slices(30.0, [&](double) -> std::optional<double>
	{
		++calls;
		return 0.0;
	});
	assert_equal(true, calls <= faces::video_slices, "a reader that answers one time over and over still ends"sv);
}

// A vector `likeness` alike to `v`, turned towards `other`.
static faces::face_vector alike_to(const faces::face_vector& v, const faces::face_vector& other, const float likeness)
{
	auto along = 0.0f;
	for (auto i = 0; i < faces::vector_size; ++i) along += v[i] * other[i];
	faces::face_vector away{};
	auto length = 0.0f;
	for (auto i = 0; i < faces::vector_size; ++i)
	{
		away[i] = other[i] - along * v[i];
		length += away[i] * away[i];
	}
	faces::face_vector result{};
	const auto aside = std::sqrt(1.0f - likeness * likeness) / std::sqrt(length);
	for (auto i = 0; i < faces::vector_size; ++i) result[i] = likeness * v[i] + aside * away[i];
	return result;
}

// docs/faces.md#33-videos: a video's frames make one record - each person once, by the clearest face of theirs, with
// the parts of the video they were seen in - and two faces in one frame are two people.
static void should_make_one_record_of_a_videos_frames()
{
	const rectd left{0.1, 0.2, 0.2, 0.3};
	const rectd right{0.6, 0.2, 0.2, 0.3};

	std::vector<faces::video_frame> frames(10);
	for (auto i = 0; i < 10; ++i)
	{
		frames[i].time = i;
		frames[i].found.extent = {1280, 720};
	}
	const auto add = [&frames](const int frame, const faces::found_face& face)
	{
		frames[frame].found.faces.push_back(face);
	};

	// One person in frames 0 to 4 but 3, gone for 5 and 6, back for 7 to 9, and clearest in 8; another beside them from
	// 1 to 8; a face no hash describes; and in frame 6 two faces alike to one first seen in frame 5.
	for (const auto i : {0, 1, 2, 4, 7, 8, 9}) add(i, face_like(axis_vector(3, 0.05f, 100 + i), left));
	frames[8].found.faces.back().eye_distance = 120;
	for (auto i = 1; i <= 8; ++i) add(i, face_like(axis_vector(9, 0.05f, 200 + i), right));
	auto blank = face_like(axis_vector(20, 0.05f, 1), {0.4, 0.6, 0.1, 0.1});
	blank.hash = {};
	add(5, blank);
	add(5, face_like(axis_vector(15, 0.05f, 300), {0.4, 0.6, 0.1, 0.1}));
	add(6, face_like(axis_vector(15, 0.05f, 301), {0.4, 0.6, 0.1, 0.1}));
	add(6, face_like(axis_vector(15, 0.05f, 302), {0.1, 0.6, 0.1, 0.1}));

	const auto merged = faces::merge_video_frames(frames, 10.0);
	assert_equal(true, merged.video && merged.status == faces::record_status::read, "a video's record"sv);
	assert_equal(1280, merged.extent.cx, "measured on its frames"sv);
	assert_equal(4, static_cast<int>(merged.faces.size()),
	             "each person once, two alike in one frame as two, and nothing a hash does not describe"sv);

	auto ordered = true;
	for (size_t i = 1; i < merged.faces.size(); ++i)
		ordered = ordered && merged.faces[i - 1].time_ms <= merged.faces[i].time_ms;
	assert_equal(true, ordered, "in the order they are seen clearest"sv);

	const auto person = [&merged](const rectd& box, const uint32_t time_ms) -> const faces::found_face*
	{
		for (const auto& f : merged.faces)
			if (f.box == box && f.time_ms == time_ms) return &f;
		return nullptr;
	};

	const auto* const first = person(left, 8'000);
	assert_equal(true, first != nullptr, "the first by their clearest face, in frame 8"sv);
	assert_equal(true, first && first->seen == std::vector<faces::time_span>{{0, 4'500}, {6'500, 10'000}},
	             "seen until a frame read after the last with them, through one read without them, and again"sv);

	const auto* const beside = person(right, 1'000);
	assert_equal(true, beside && beside->seen == std::vector<faces::time_span>{{500, 8'500}},
	             "the one beside them, seen throughout"sv);

	auto alike = 0;
	for (const auto& f : merged.faces)
		if (f.time_ms >= 5'000 && f.time_ms <= 6'000 && f.box.Y == 0.6) ++alike;
	assert_equal(2, alike, "two faces alike in one frame are two people"sv);

	// Turned away where they stood a frame before, someone is still themselves at a lower likeness; the same likeness
	// elsewhere in the picture is someone else.
	const auto a = axis_vector(3, 0.02f, 400);
	std::vector<faces::video_frame> turning(3);
	for (auto i = 0; i < 3; ++i) turning[i].time = i;
	turning[0].found.faces = {face_like(a, left)};
	turning[1].found.faces = {face_like(alike_to(a, axis_vector(30, 0.02f, 401), 0.5f), left)};
	turning[2].found.faces = {face_like(alike_to(a, axis_vector(40, 0.02f, 402), 0.5f), right)};
	assert_equal(2, static_cast<int>(faces::merge_video_frames(turning, 3.0).faces.size()),
	             "someone turning where they stood is one person; a like face elsewhere another"sv);

	// Glimpsed once and unclear where frames read either side would have shown it again, a face is the detector's
	// mistake; clear, or in a video read seconds apart, it is kept.
	std::vector<faces::video_frame> close(5);
	for (auto i = 0; i < 5; ++i) close[i].time = i * 0.2;
	close[2].found.faces = {face_like(axis_vector(50, 0.02f, 600), left, false),
	                        face_like(axis_vector(60, 0.02f, 601), right)};
	const auto glimpsed = faces::merge_video_frames(close, 1.0);
	assert_equal(true, glimpsed.faces.size() == 1 && glimpsed.faces[0].box == right,
	             "an unclear face glimpsed once is left out, a clear one kept"sv);
	std::vector<faces::video_frame> sparse(3);
	for (auto i = 0; i < 3; ++i) sparse[i].time = i * 30.0;
	sparse[1].found.faces = {face_like(axis_vector(50, 0.02f, 602), left, false)};
	assert_equal(1, static_cast<int>(faces::merge_video_frames(sparse, 90.0).faces.size()),
	             "and an unclear one seen once where the frames read are far apart"sv);

	// The clearest, where a crowd holds more than a record keeps.
	std::vector<faces::video_frame> crowd(1);
	for (auto i = 0; i < 70; ++i)
	{
		auto f = face_like(axis_vector(i, 0.01f, 500 + i), {0.01 * i, 0.5, 0.01, 0.01});
		f.eye_distance = static_cast<uint16_t>(30 + i);
		crowd[0].found.faces.push_back(f);
	}
	const auto kept = faces::merge_video_frames(crowd, 1.0);
	assert_equal(faces::defaults().max_faces, static_cast<int>(kept.faces.size()), "a record keeps 64 faces"sv);
	assert_equal(true, std::ranges::all_of(kept.faces, [](const faces::found_face& f) { return f.eye_distance >= 36; }),
	             "the clearest"sv);

	std::vector<faces::video_frame> broken(2);
	for (auto& f : broken) f.found.status = faces::record_status::undecodable;
	assert_equal(true, faces::merge_video_frames(broken, 2.0).status == faces::record_status::undecodable,
	             "a video none of whose frames decoded is undecodable"sv);
	assert_equal(true, faces::merge_video_frames({}, 2.0).status == faces::record_status::undecodable,
	             "as is one with no frame read"sv);
}

static void should_read_a_pictures_faces_into_its_record()
{
	libmug::face_detector detector;
	libmug::face_embedder embedder;
	assert_equal(true, embedder.ready(), "the network loads"sv);

	const auto portrait = decode_picture(face_sample("test.jpg"));
	assert_equal(true, core::is_valid(portrait), "libfacedetection's portrait decodes"sv);

	const auto record = faces::read_record(detector, embedder, portrait, {});
	assert_equal(1, static_cast<int>(record.faces.size()), "one face"sv);
	assert_equal(record.model, faces::model_id(), "measured by this build"sv);
	assert_equal(true, !record.faces[0].hash.is_empty(), "embedded"sv);

	// The same picture decoded smaller is the same person, and the same face.
	const auto smaller = decode_picture(face_sample("test.jpg"), false, {160, 160});
	const auto again = faces::read_record(detector, embedder, smaller, {});
	assert_equal(1, static_cast<int>(again.faces.size()), "found again"sv);
	const auto s = faces::similarity(record.faces[0].hash, again.faces[0].hash);
	assert_equal(true, s >= faces::defaults().similar, std::format("similarity {}", s),
	             "the same face resembles itself"sv);
	assert_same_area(record.faces[0].box, again.faces[0].box, "and is in the same place"sv, 0.03);

	// A named region where the detector found nothing is looked in again; one over the face is that face.
	std::vector<faces::named_region> regions{{"Portrait", record.faces[0].box}};
	const auto named = faces::read_record(detector, embedder, portrait, regions);
	assert_equal(1, static_cast<int>(named.faces.size()), "a region over a found face adds none"sv);
	assert_equal(0, faces::match_regions(regions, named.faces)[0], "the region is the face"sv);

	const auto nothing = faces::read_record(detector, embedder, nullptr, {});
	assert_equal(true, nothing.status == faces::record_status::undecodable, "no picture reads as undecodable"sv);
}

static void should_recover_small_faces_without_replacing_primary_detections()
{
	const auto group = decode_picture(face_sample("test2.jpg"));
	assert_equal(true, core::is_valid(group) && core::is_packed(group->format()), "the group photo decodes");
	const sizei extent{1200, 900};
	const auto scene = std::make_shared<core::surface>();
	assert_equal(true, scene->alloc(extent, group->format()), "a larger picture to place the group in");
	scene->clear(core::rgba(128, 128, 128));
	const auto size = group->dimensions();
	const auto x = (extent.cx - size.cx) / 2;
	const auto y = (extent.cy - size.cy) / 2;
	for (auto row = 0; row < size.cy; ++row)
		std::memcpy(scene->pixels() + (y + row) * scene->stride() + x * 4, group->pixels() + row * group->stride(),
		            size.cx * 4);

	libmug::face_detector detector;
	libmug::face_embedder embedder;
	faces::tuning tuning;
	tuning.detector_edge = 384;
	const auto original = faces::find_faces(detector, group);
	const auto first = faces::find_faces(detector, scene, tuning);
	assert_equal(true, !first.faces.empty() && first.faces.size() < original.faces.size(),
	             "the smaller plane has enough evidence to try again, but misses some faces");
	const auto record = faces::read_record(detector, embedder, scene, {}, tuning);
	assert_equal(original.faces.size(), record.faces.size(), "each face is found once after the second look");
	for (const auto& f : original.faces)
	{
		const rectd box{(f.bounds.X + x) / extent.cx, (f.bounds.Y + y) / extent.cy, f.bounds.Width / extent.cx,
		                f.bounds.Height / extent.cy};
		assert_equal(true,
		             std::ranges::any_of(record.faces,
		                                 [&](const auto& found) { return box_overlap(box, found.box) > 0.5; }),
		             "the recovered face is in the right place");
	}
	for (size_t i = 0; i < first.faces.size(); ++i)
	{
		faces::face_vector v{};
		assert_equal(true, faces::embed_face(embedder, scene, first.faces[i].marks, v), "the first detection embeds");
		assert_equal(true, record.faces[i].hash == faces::quantize(v),
		             "the initial face's exact signature and position survive");
	}
}

static void should_keep_faces_in_the_store_by_picture()
{
	const auto a = axis_vector(5, 0.05f, 11);
	const auto b = axis_vector(40, 0.05f, 12);
	const auto first = core::file_path(core::folder_path("C:\\pictures"), "one.jpg");
	const auto second = core::file_path(core::folder_path("C:\\pictures"), "two.jpg");

	faces::face_record r1;
	r1.model = faces::model_id();
	r1.faces = {face_like(a, {0.1, 0.1, 0.2, 0.2}), face_like(b, {0.6, 0.1, 0.2, 0.2})};
	faces::face_record r2;
	r2.model = faces::model_id();
	r2.faces = {face_like(axis_vector(5, 0.05f, 13), {0.4, 0.4, 0.2, 0.2})};

	faces::face_store store;
	store.add(first, r1);
	store.add(second, r2);
	store.publish();

	assert_equal(3u, store.face_count(), "three faces"sv);
	assert_equal(true, store.find(first).has_value() && store.find(second).has_value(), "found by picture"sv);
	assert_equal(1u, *store.face_at(first, 1), "a face by its picture and position"sv);

	const auto similar = faces::similar_faces(store, faces::quantize(a), faces::defaults().similar);
	assert_equal(2, static_cast<int>(similar.size()), "the face and its likeness, not the other face"sv);
	assert_equal(0u, similar[0].face, "itself strongest"sv);
	assert_equal(2u, similar[1].face, "then its likeness in the other picture"sv);

	// A new record replaces the picture's run, and the old one is compared with nothing.
	r1.faces[0].flags |= faces::face_flag::hidden;
	store.add(first, r1);
	store.publish();
	assert_equal(5u, store.face_count(), "appended, never moved"sv);
	assert_equal(true, store.record(first)->faces[0].is_hidden(), "the picture's record is the new one"sv);
	const auto after = faces::similar_faces(store, faces::quantize(a), faces::defaults().similar);
	assert_equal(2, static_cast<int>(after.size()), "the replaced faces are not found"sv);

	store.retire(second);
	assert_equal(false, store.find(second).has_value(), "a retired picture has no faces"sv);
}

// docs/faces.md#32-the-scan: the scan puts away the faces of a picture the index no longer holds, so no page or group
// shows a face whose picture is gone, and leaves those of a picture it holds.
static void should_put_away_the_faces_of_pictures_the_index_no_longer_holds()
{
	const auto folder = test_temps.next_folder("faces-gone");
	const auto path = test_temps.next_path_in(folder, ".jpg");
	platform::copy_file(test_files_folder().combine_file("Test.jpg"), path, false, false);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	core::index_roots roots;
	roots.folders.emplace(folder);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.cache_load_complete();
	index.scan_uncached(test_token);

	const auto record_of = [](const core::date_t stamp)
	{
		faces::face_record r;
		r.model = faces::model_id();
		r.stamp = stamp;
		r.faces = {face_like(axis_vector(5, 0.05f, 11), {0.1, 0.1, 0.2, 0.2})};
		return faces::pack_record(r);
	};

	const auto gone = core::file_path(folder, "gone.jpg");
	auto& search = index.faces();
	search.configure(true, true, false);
	search.load(path, record_of(index.find_item(path).file_modified.load()));
	search.load(gone, record_of(core::date_t(2020, 1, 1)));
	search.loaded();
	search.start_scan();

	const auto store = search.store();
	assert_equal(true, store->find(path).has_value(), "the picture the index holds keeps its faces"sv);
	assert_equal(false, store->find(gone).has_value(), "the one it no longer holds has them put away"sv);
	assert_equal(1u, search.progress().done, "with nothing left to read"sv);
}

// A video of `frames` a second apiece - each a picture of `extent`, or grey where there is none - written as YUV4MPEG2,
// which FFmpeg reads with no encoder, every frame a key frame.
static void write_y4m(const core::file_path path, const std::vector<core::const_surface_ptr>& frames,
                      const sizei extent)
{
	std::string out = std::format("YUV4MPEG2 W{} H{} F1:1 Ip A1:1 C420jpeg\n", extent.cx, extent.cy);
	const auto luma = static_cast<size_t>(extent.cx) * extent.cy;
	std::vector<uint8_t> y(luma), u(luma / 4), v(luma / 4);

	for (const auto& frame : frames)
	{
		std::ranges::fill(y, uint8_t{128});
		std::ranges::fill(u, uint8_t{128});
		std::ranges::fill(v, uint8_t{128});

		if (frame)
		{
			// BT.601 at video range, from the decode's B, G and R.
			const auto bgr = [&frame](const int x, const int row)
			{
				const auto* const p = frame->pixels() + frame->stride() * row + static_cast<size_t>(x) * 4;
				return std::array{static_cast<double>(p[2]), static_cast<double>(p[1]), static_cast<double>(p[0])};
			};

			for (auto row = 0; row < extent.cy; ++row)
			{
				for (auto x = 0; x < extent.cx; ++x)
				{
					const auto [r, g, b] = bgr(x, row);
					y[static_cast<size_t>(row) * extent.cx + x] = static_cast<uint8_t>(
						std::lround(16.0 + (65.481 * r + 128.553 * g + 24.966 * b) / 255.0));
				}
			}

			for (auto row = 0; row < extent.cy / 2; ++row)
			{
				for (auto x = 0; x < extent.cx / 2; ++x)
				{
					std::array<double, 3> sum{};
					for (auto dy = 0; dy < 2; ++dy)
					{
						for (auto dx = 0; dx < 2; ++dx)
						{
							const auto c = bgr(x * 2 + dx, row * 2 + dy);
							for (auto i = 0; i < 3; ++i) sum[i] += c[i] / 4.0;
						}
					}
					const auto [r, g, b] = sum;
					const auto at = static_cast<size_t>(row) * (extent.cx / 2) + x;
					u[at] = static_cast<uint8_t>(std::lround(128.0 + (-37.797 * r - 74.203 * g + 112.0 * b) / 255.0));
					v[at] = static_cast<uint8_t>(std::lround(128.0 + (112.0 * r - 93.786 * g - 18.214 * b) / 255.0));
				}
			}
		}

		out += "FRAME\n";
		for (const auto* plane : {&y, &u, &v}) out.append(std::bit_cast<const char*>(plane->data()), plane->size());
	}

	write_test_file(path, out);
}

// docs/faces.md#33-videos: a video's faces are read from its key frames, each person once, with where they are seen,
// while face search reads videos; and a kind it does not read is put away as it loads.
static void should_read_a_videos_faces_from_its_key_frames()
{
	const auto portrait = decode_picture(face_sample("test.jpg"), false, {320, 320});
	assert_equal(true, core::is_valid(portrait) && core::is_packed(portrait->format()), "the portrait decodes"sv);
	const auto dims = portrait->dimensions();
	const sizei extent{dims.cx & ~1, dims.cy & ~1};

	const auto folder = test_temps.next_folder("faces-video");
	const auto path = folder.combine_file("clip.y4m");
	const core::const_surface_ptr face = portrait;
	write_y4m(path, {face, face, nullptr, nullptr, nullptr, face, face, face}, extent);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	core::index_roots roots;
	roots.folders.emplace(folder);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.cache_load_complete();
	index.scan_uncached(test_token);

	auto& search = index.faces();
	search.configure(false, true, false);
	search.loaded();
	search.start_scan();

	const auto store = search.store();
	const auto record = store ? store->record(path) : std::nullopt;
	assert_equal(true, record.has_value(), "the video is read"sv);
	assert_equal(true, record->video && record->status == faces::record_status::read, "as a video"sv);
	assert_equal(1, static_cast<int>(record->faces.size()), "the one person in it, once"sv);
	assert_equal(0u, record->faces[0].time_ms, "found in its first frame"sv);
	assert_equal(true, record->faces[0].seen == std::vector<faces::time_span>{{0, 1'500}, {4'500, 8'000}},
	             "seen at its start, gone, and back to its end"sv);

	libmug::face_detector detector;
	libmug::face_embedder embedder;
	const auto still = faces::read_record(detector, embedder, portrait, {});
	assert_equal(1, static_cast<int>(still.faces.size()), "the portrait's own face"sv);
	const auto likeness = faces::similarity(still.faces[0].hash, record->faces[0].hash);
	assert_equal(true, likeness >= faces::defaults().confident, std::format("likeness {}", likeness),
	             "is the face its frames hold"sv);

	files ff;
	const auto frame = faces::decode_face_source(ff, path, index.find_item(path).ft, true, record->faces[0].time_ms,
	                                             {160, 160});
	assert_equal(true, core::is_valid(frame) && std::max(frame->dimensions().cx, frame->dimensions().cy) == 160,
	             "the frame a face was found in is read again to cut it from"sv);

	// Put away while videos are not read: as face search loads, a video's record is stepped over.
	null_async_strategy photos_as;
	index_state photos_only(photos_as, locations);
	auto& photo_search = photos_only.faces();
	photo_search.configure(true, false, false);
	photo_search.load(path, faces::pack_record(*record));
	photo_search.loaded();
	assert_equal(false, photo_search.store()->find(path).has_value(),
	             "a video is put away while videos are not read"sv);
}

// docs/faces.md#32-the-scan: what the workers read joins the store a few times a second, and the window is told then,
// rather than as each picture is read, which would redraw the Faces view a hundred times a second through a scan.
static void should_tell_the_window_of_face_scan_progress_a_few_times_a_second()
{
	const auto folder = test_temps.next_folder("faces-scan-pace");
	constexpr int pictures = 80;
	for (int i = 0; i < pictures; ++i)
		platform::copy_file(test_files_folder().combine_file("Test.jpg"), test_temps.next_path_in(folder, ".jpg"),
		                    false, false);

	// The face workers on threads of their own, each picture taking a while - staggered, so they finish one at a time,
	// as a scan's do - and everything else at once.
	class pooled_async_strategy final : public null_async_strategy
	{
		std::mutex _mutex;
		std::condition_variable _ready;
		std::deque<std::function<void()>> _work;
		bool _stopping = false;
		int _taken = 0;
		std::vector<std::jthread> _threads;

	public:
		std::atomic_int faces_told = 0;

		explicit pooled_async_strategy(const int threads)
		{
			for (int i = 0; i < threads; ++i)
			{
				_threads.emplace_back([this]
				{
					for (;;)
					{
						std::function<void()> f;
						int taken = 0;
						{
							std::unique_lock lock(_mutex);
							_ready.wait(lock, [this] { return _stopping || !_work.empty(); });
							if (_work.empty()) return;
							f = std::move(_work.front());
							_work.pop_front();
							taken = _taken++;
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(5 + taken * 7 % 20));
						f();
					}
				});
			}
		}

		~pooled_async_strategy() override
		{
			{
				const std::lock_guard lock(_mutex);
				_stopping = true;
			}
			_ready.notify_all();
		}

		void queue_async(const async_queue q, const std::function<void()> f) override
		{
			if (q != async_queue::faces_work) return f();
			{
				const std::lock_guard lock(_mutex);
				_work.push_back(f);
			}
			_ready.notify_one();
		}

		void invalidate_view(const view_invalid invalid) override
		{
			if ((invalid & view_invalid::faces) != view_invalid::none) ++faces_told;
		}
	};

	pooled_async_strategy as(index_state::face_threads());
	const location_cache locations;
	index_state index(as, locations);
	core::index_roots roots;
	roots.folders.emplace(folder);
	index.index_roots(roots);
	index.index_folders(test_token);
	index.cache_load_complete();
	index.scan_uncached(test_token);

	auto& search = index.faces();
	as.faces_told = 0;
	const auto started_ms = core::now_ms();
	search.configure(true, true, false);
	search.loaded();
	search.start_scan();
	const auto elapsed_ms = core::now_ms() - started_ms;

	assert_equal(static_cast<uint32_t>(pictures), search.progress().done, "every picture is read"sv);
	// Four times a second while pictures are read, and a few times as face search starts, the scan ends and its
	// faces are compared - told as each picture is read, it would be told about three times in four.
	const auto most = static_cast<int>(elapsed_ms / 250) + 16;
	assert_equal(true, as.faces_told.load() <= most,
	             std::format("the window is told {} times for {} pictures in {} ms, not as each is read",
	                         as.faces_told.load(), pictures, elapsed_ms));
}

static void should_overlap_face_and_file_scans_only_on_seek_free_storage()
{
	faces::scan_io_gate gate;
	{
		const auto index = gate.start_index(true);
		assert_equal(true, index != nullptr && gate.try_start_face(true) != nullptr,
		             "two known seek-free workloads can overlap");
		assert_equal(false, gate.try_start_face(false) != nullptr, "unverified face storage cannot overlap");
	}

	auto face = gate.try_start_face(true);
	assert_equal(true, face != nullptr, "the face scan has a picture in flight");
	std::atomic_int entered = 0;
	std::jthread index([&]
	{
		const auto regular = gate.start_index();
		entered = regular ? 1 : -1;
		entered.notify_all();
		entered.wait(1);
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	auto blocked = false;
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (!gate.try_start_face(true))
		{
			blocked = true;
			break;
		}
		std::this_thread::yield();
	}
	const auto started_too_soon = entered.load() != 0;
	face.reset();
	entered.wait(0);
	const auto was_exclusive = entered.load() == 1 && !gate.try_start_face(true);
	entered = 2;
	entered.notify_all();
	index.join();

	assert_equal(true, blocked && !started_too_soon,
	             "the regular index waits for current work and blocks new face reads");
	assert_equal(true, was_exclusive, "the slow regular index has the disk to itself");
	assert_equal(true, gate.try_start_face(false) != nullptr, "face reads resume when the regular index releases");
}

static std::optional<faces::placed_faces> edit_faces(faces::face_book& book, std::span<const faces::face_ref> refs,
                                                     const faces::face_action action, const uint32_t person = 0)
{
	const std::array batch{faces::placement_batch{person, {}, {refs.begin(), refs.end()}}};
	return book.place(batch, action, book.generation());
}

static faces::face_signature test_signature(const int axis)
{
	return {0xfedc1234, faces::quantize(axis_vector(axis, 0.05f, 7))};
}

static std::string legacy_face_header(const int version)
{
	return std::format("Diffractor faces,{},,,,,,,,,,\r\n"
	                   "type,person,name,file,x,y,width,height,position,confirmed,hidden,rejected\r\n",
	                   version);
}

static void should_round_trip_the_face_book_as_csv()
{
	const auto folder = test_temps.next_folder("faces-csv");
	const auto path = folder.combine_file("one, two.jpg");
	const auto key = test_signature(7);
	const std::array refs{faces::face_ref{path, {0.123456789012345, 0.2, 0.3, 0.4}, 0, key}};
	const std::array measured_again{faces::face_ref{path, {0.123456789012346, 0.201, 0.3, 0.4}, 0, key}};
	const auto quoted_name = "Ann, \"Annie\"\r\nDoe \xc3\xa9"s;
	using enum faces::face_action;
	faces::face_book book;
	const auto zac = book.create("Zac");
	const auto tom = book.create("Tom");
	const auto alex = book.create("Alex");
	book.create(quoted_name);
	assert_equal(0u, book.create(" "), "only named people enter the book"sv);
	assert_equal(tom, book.create(" tOm "), "one stable identity per name"sv);
	assert_equal(false, book.rename(alex, "Zac"), "rename refuses a collision"sv);
	edit_faces(book, refs, reject, zac);
	edit_faces(book, measured_again, reject, tom);
	edit_faces(book, refs, hide);
	assert_equal(true, book.marks(path).size() == 1 && book.marks(path).front().hidden,
	             "one face found again and hidden"sv);
	edit_faces(book, refs, unhide);
	edit_faces(book, refs, assign, alex);
	assert_equal(true, book.marks(path).front().rejected == std::vector{zac, tom} && !book.marks(path).front().hidden,
	             "showing and assigning to someone else preserve both rejections"sv);
	edit_faces(book, refs, hide);
	const std::array legacy{faces::face_ref{folder.combine_file("private.jpg"), refs[0].box, 0}};
	edit_faces(book, legacy, hide);
	size_t omitted = 0;
	const auto csv = book.to_csv(&omitted);
	assert_equal(true,
	             omitted == 1 && csv.find("private.jpg") == std::string::npos &&
	                 csv.find("one, two.jpg") == std::string::npos && csv.find("file,") == std::string::npos,
	             "no file locators in the backup; the unportable decision is counted and still held locally"sv);
	assert_equal(true, csv.find("\"Ann, \"\"Annie\"\"\r\nDoe \xc3\xa9\"") != std::string::npos,
	             "quoted multiline UTF-8 name, even with no marked faces"sv);

	faces::face_book restored;
	const auto local_tom = restored.create("Tom"); // The imported ID for Zac already belongs to Tom here.
	auto result = restored.import_csv("\xef\xbb\xbf" + csv);
	assert_equal(true, result.ok && result.faces == 1 && result.people_added == 3 && result.people_joined == 1,
	             "CSV merges by name, not by another book's IDs"sv);
	if (!result.ok) return;
	const auto people = restored.people();
	const auto local_zac = people->named("Zac")->id;
	const auto local_alex = people->named("Alex")->id;
	const auto mark = restored.signature_mark(key);
	assert_equal(true,
	             restored.marks(path).empty() && restored.mark_count() == 1 && mark && mark->hidden &&
	                 mark->person == local_alex && mark->rejected == std::vector{local_tom, local_zac},
	             "hidden, Alex, and both rejected identities survive together"sv);
	auto wrong = key;
	++wrong.model;
	assert_equal(false, restored.signature_mark(wrong).has_value(), "the same bytes from another model do not match"sv);
	wrong = key;
	wrong.hash.values.back() ^= 1;
	assert_equal(false, restored.signature_mark(wrong).has_value(), "even the last byte must match"sv);
	wrong = key;
	wrong.hash.scale = std::bit_cast<float>(std::bit_cast<uint32_t>(key.hash.scale) + 1);
	assert_equal(false, restored.signature_mark(wrong).has_value(), "the scale must match bit for bit"sv);
	const auto before = restored.to_csv();
	const auto move = edit_faces(restored, refs, assign, local_zac);
	const auto moved = restored.marks(path);
	assert_equal(true,
	             move.has_value() && moved.front().person == local_zac && !moved.front().hidden &&
	                 moved.front().rejected == std::vector{local_tom},
	             "only deliberate assignment to Zac overrides Not Zac; Not Tom remains"sv);
	assert_equal(true, move && restored.undo(*move) && restored.to_csv() == before,
	             "undo restores the previous person, visibility and both rejections"sv);
	assert_equal(true, restored.import_csv(csv).ok && restored.to_csv() == before,
	             "importing the same backup twice is idempotent"sv);
	assert_equal(true,
	             restored.rename(local_tom, "Thomas") && restored.people()->named("Thomas")->id == local_tom &&
	                 restored.signature_mark(key)->rejected == std::vector{local_tom, local_zac},
	             "renaming a rejected person does not lose the evidence"sv);
	restored.rename(local_tom, "Tom");

	edit_faces(restored, refs, reject, local_alex);
	assert_equal(true,
	             restored.import_csv(csv).ok && !restored.marks(path).front().person &&
	                 restored.marks(path).front().rejected.size() == 3,
	             "an older backup cannot override a local rejection"sv);

	edit_faces(restored, refs, assign, local_tom);
	edit_faces(restored, refs, hide);
	assert_equal(true,
	             restored.import_csv(csv).ok && restored.signature_mark(key)->person == local_tom &&
	                 restored.signature_mark(key)->hidden,
	             "import cannot replace or reject a locally confirmed person"sv);
}

static void should_refuse_invalid_face_csv_without_changing_the_book()
{
	faces::face_book book;
	book.create("Keep me");
	const auto before = book.to_csv();
	const auto generation = book.generation();
	auto persisted = false;
	book.persist_to([&](faces::book_write) { persisted = true; });
	const auto header = legacy_face_header(3);
	const auto person = "person,10,Alice,,,,,,,,,\r\n"s;
	const auto path = core::windows_path_semantics ? "C:\\Photos\\one.jpg"sv : "/photos/one.jpg"sv;
	const auto face = std::format("face,,,{},0.1,0.2,0.3,0.4,0,", path);
	for (const auto& bad :
	     {person, "person,20,aLiCe,,,,,,,,,\r\n"s, "group,10,2,,,,,,,,,\r\n"s, "person,20,\"unterminated,,,,,,,,,\r\n"s,
	      "alias,99,Former,,,,,,,,,\r\n"s, "alias,10,aLiCe,,,,,,,,,\r\n"s, face + "99,0,\r\n", face + ",0,99\r\n",
	      face + "10,0,10\r\n", face + ",0,10 10\r\n", face + ",0,0\r\n", face + ",2,\r\n",
	      std::format("face,,,{},nan,0.2,0.3,0.4,0,,0,\r\n", path),
	      std::format("face,,,{},0.1,0.2,-0.3,0.4,0,,0,\r\n", path),
	      std::format("face,,,{},0.1,0.2,0.3,0.4,256,,0,\r\n", path)})
	{
		const auto result = book.import_csv(header + person + bad);
		assert_equal(true, !result.ok && !result.error.empty(), "malformed or inconsistent CSV refused"sv);
	}
	for (const char version : {'1', '9'})
	{
		auto unsupported = header;
		unsupported[unsupported.find(',') + 1] = version;
		const auto result = book.import_csv(unsupported);
		assert_equal(true, !result.ok && result.error.find("version") != std::string::npos,
		             "older and newer formats explicitly refused"sv);
	}
	const auto v4 = faces::face_book{}.to_csv() + "person,10,Alice,,,,,,\r\n";
	const auto values = std::string(2 * faces::vector_size, 'a');
	const auto signature = [&](const std::string_view model, const std::string_view scale, const std::string_view bytes,
	                           const std::string_view state = "10,0,")
	{
		return std::format("signature,,,{},{},{},{}\r\n", model, scale, bytes, state);
	};
	const auto row = signature("5", "3f800000", values);
	for (const auto& bad :
	     {signature("0", "3f800000", values), signature("4294967296", "3f800000", values),
	      signature("5", "00000000", values), signature("5", "bf800000", values), signature("5", "7fc00000", values),
	      signature("5", "7f800000", values), signature("5", "3f80000!", values),
	      signature("5", "3f800000", values + "00"), signature("5", "3f800000", values.substr(1)),
	      signature("5", "3f800000", values.substr(1) + "z"), signature("5", "3f800000", values, ",0,99"),
	      signature("5", "3f800000", values, "10,0,10"), row + signature("5", "3f800000", values, ",1,"),
	      face + "10,1,\r\n"})
		assert_equal(false, book.import_csv(v4 + bad).ok,
		             "invalid signature data or legacy locator refused atomically"sv);
	assert_equal(true, !persisted && generation == book.generation() && before == book.to_csv(),
	             "failed imports publish nothing, even after valid records"sv);
}

// docs/faces.md#62-marks: the marks on a picture's faces go with it as Diffractor renames, moves and turns it, and with
// each face as an edit to its pixels moves the face: to the face found again that resembles its own.
static void should_keep_marks_with_pictures_as_they_move_and_change()
{
	const auto folder = core::folder_path("C:\\pictures");
	const auto at = [&folder](const std::string_view name)
	{
		return core::file_path(folder, name);
	};
	const rectd box{0.1, 0.2, 0.2, 0.3};

	faces::face_book book;
	std::vector<faces::book_write> written;
	book.persist_to([&written](faces::book_write w) { written.push_back(std::move(w)); });
	const auto jane = book.create("Jane");
	const auto mark = [&book, &box, jane](const core::file_path path, const faces::face_action kind)
	{
		const std::array refs{faces::face_ref{path, box, 0}};
		edit_faces(book, refs, kind, jane);
	};
	const auto kind_at = [&book](const core::file_path path)
	{
		const auto marks = book.marks(path);
		return marks.size() == 1
		           ? std::optional(marks.front().hidden ? faces::face_action::hide : faces::face_action::assign)
		           : std::nullopt;
	};

	// Renamed, a picture takes its marks with it, and the database is told of both its paths.
	mark(at("a.jpg"), faces::face_action::assign);
	book.moved(std::array{std::pair{at("a.jpg"), at("b.jpg")}});
	assert_equal(true, book.marks(at("a.jpg")).empty() && kind_at(at("b.jpg")) == faces::face_action::assign,
	             "a picture renamed keeps its marks"sv);
	assert_equal(2, static_cast<int>(written.back().pictures.size()), "the old path given up, the new one written"sv);

	// Moved together, two pictures that swap names keep their own; one moved onto another's path takes its place.
	mark(at("c.jpg"), faces::face_action::hide);
	book.moved(std::array{std::pair{at("b.jpg"), at("c.jpg")}, std::pair{at("c.jpg"), at("b.jpg")}});
	assert_equal(true,
	             kind_at(at("b.jpg")) == faces::face_action::hide && kind_at(at("c.jpg")) == faces::face_action::assign,
	             "two pictures that swapped names keep their own marks"sv);
	book.moved(std::array{std::pair{at("unmarked.jpg"), at("c.jpg")}});
	assert_equal(true, book.marks(at("c.jpg")).empty(), "a picture replaced loses the marks of the one it replaced"sv);

	// A folder moved takes the marks of every picture in it, at any depth, and of none beside it.
	const auto trip = folder.combine("2019").combine("trip");
	mark(core::file_path(trip, "d.jpg"), faces::face_action::assign);
	book.moved(std::array{std::pair{folder.combine("2019"), core::folder_path("D:\\archive\\2019")}});
	assert_equal(true,
	             book.marks(core::file_path(trip, "d.jpg")).empty() &&
	                 kind_at(core::file_path(core::folder_path("D:\\archive\\2019\\trip"), "d.jpg")) ==
	                     faces::face_action::assign,
	             "a folder moved takes the marks of the pictures in it"sv);
	assert_equal(true, kind_at(at("b.jpg")) == faces::face_action::hide, "and leaves those beside it"sv);

	// Turned by its declared orientation alone, a picture's marks turn with it, as its face record does.
	book.turned(at("b.jpg"), core::orientation::top_left, core::orientation::right_top);
	assert_same_area({0.5, 0.1, 0.3, 0.2}, book.marks(at("b.jpg")).front().box, "a mark turned with its picture"sv);

	// Its pixels turned and cropped, a picture is read again: Jane's face is found elsewhere, resembling itself, and
	// someone else stands where the hidden face was; an unclear face nobody could compare waits where it was.
	const auto edited = at("edited.jpg");
	const rectd place_a{0.1, 0.1, 0.2, 0.2};
	const rectd place_b{0.6, 0.1, 0.2, 0.2};
	const rectd place_c{0.1, 0.6, 0.2, 0.2};
	const rectd place_d{0.6, 0.6, 0.2, 0.2};
	const rectd place_e{0.35, 0.35, 0.1, 0.1};
	faces::found_face unclear;
	unclear.box = place_c;
	const std::vector before{face_like(axis_vector(1, 0.02f, 1), place_a), face_like(axis_vector(2, 0.02f, 2), place_b),
	                         unclear};
	const std::array marked{faces::face_ref{edited, place_a, 0}};
	const auto wrong = book.create("Wren");
	edit_faces(book, marked, faces::face_action::reject, wrong);
	edit_faces(book, marked, faces::face_action::assign, jane);
	const std::array put_away{faces::face_ref{edited, place_b, 1}, faces::face_ref{edited, place_c, 2}};
	edit_faces(book, put_away, faces::face_action::hide);

	faces::found_face unclear_moved;
	unclear_moved.box = place_e;
	const std::vector after{face_like(axis_vector(3, 0.02f, 3), place_b), face_like(axis_vector(1, 0.02f, 4), place_d),
	                        unclear_moved};
	book.follow(edited, before, after);
	const auto followed = book.marks(edited);
	const auto found = [&followed](const rectd& place)
	{
		return std::ranges::find_if(followed, [&place](const faces::face_mark& m) { return m.box == place; });
	};
	assert_equal(true, found(place_d) != followed.end() && found(place_d)->person == jane,
	             "Jane's mark goes to her face, found elsewhere"sv);
	assert_equal(1, found(place_d) != followed.end() ? found(place_d)->position : -1, "at its place in the order"sv);
	assert_equal(true, found(place_d) != followed.end() && found(place_d)->rejected == std::vector{wrong},
	             "the face's independent rejection follows the pixels too"sv);
	assert_equal(true, found(place_b) == followed.end(), "the hidden face's mark goes rather than hide someone else"sv);
	assert_equal(true, found(place_c) != followed.end(), "the unclear face's mark waits where it was"sv);
	assert_equal(2, static_cast<int>(followed.size()), "nothing else"sv);

	// An edit that moved no face - colours changed - leaves every mark on its own face, however much it changed one.
	const auto recoloured = at("recoloured.jpg");
	const std::array kept{faces::face_ref{recoloured, place_a, 0}};
	edit_faces(book, kept, faces::face_action::assign, jane);
	book.follow(recoloured, std::vector{face_like(axis_vector(1, 0.02f, 5), place_a)},
	            std::vector{face_like(axis_vector(6, 0.02f, 6), place_a)});
	assert_equal(true, kind_at(recoloured) == faces::face_action::assign, "a face changed in place keeps its mark"sv);

	// Nor does one that brings another face to light: an unclear face put away stays put away.
	const auto brightened = at("brightened.jpg");
	const std::array hidden_unclear{faces::face_ref{brightened, place_c, 0}};
	edit_faces(book, hidden_unclear, faces::face_action::hide);
	book.follow(brightened, std::vector{unclear}, std::vector{unclear, face_like(axis_vector(8, 0.02f, 8), place_d)});
	assert_equal(true, kind_at(brightened) == faces::face_action::hide,
	             "an edit that finds another face leaves the marks of those in place"sv);

	// Turned on its side, where the detector finds no face, and back again: the mark waits, and is its face's again.
	const auto sideways = at("sideways.jpg");
	const std::array upright_face{faces::face_ref{sideways, place_a, 0}};
	edit_faces(book, upright_face, faces::face_action::assign, jane);
	const std::vector upright{face_like(axis_vector(1, 0.02f, 7), place_a)};
	book.follow(sideways, upright, std::vector<faces::found_face>{});
	assert_equal(true, kind_at(sideways) == faces::face_action::assign, "a mark waits while its face is lost"sv);
	book.follow(sideways, std::vector<faces::found_face>{}, upright);
	assert_equal(true, kind_at(sideways) == faces::face_action::assign && book.marks(sideways).front().box == place_a,
	             "and is its face's again when it is found"sv);
}

static void should_keep_the_face_book_in_its_own_database()
{
	const auto db_path = test_temps.next_path(".db");
	const auto folder = test_temps.next_folder("faces-db");
	const auto path = folder.combine_file("One.jpg");
	const std::array refs{faces::face_ref{path, {0.25, 0.5, 0.125, 0.25}, 1, test_signature(5)}};
	// A variant path reaches this same local mark on Windows; Linux's different picture has no signature.
	const std::array variant{faces::face_ref{folder.combine_file("ONE.JPG"), refs[0].box, 1,
	                                         core::case_insensitive_path_identity ? refs[0].signature : std::nullopt}};
	using enum faces::face_action;
	std::string before;
	{
		face_database db;
		assert_equal(true, db.open(db_path), "the face database opens"sv);
		faces::face_book book;
		book.persist_to([&db](const faces::book_write& w) { assert_equal(true, db.write(w), "write commits"sv); });
		edit_faces(book, refs, reject, book.create("Zac"));
		edit_faces(book, refs, reject, book.create("Tom"));
		edit_faces(book, refs, hide);
		edit_faces(book, refs, unhide);
		edit_faces(book, refs, assign, book.create("Alex"));
		edit_faces(book, variant, hide);
		before = book.to_csv();
	}
	face_database db;
	assert_equal(true, db.open(db_path), "the database reopens"sv);
	faces::face_book book;
	assert_equal(true, db.load(book), "the person schema loads"sv);
	assert_equal(before, book.to_csv(), "every decision including the rejected set survived on disk"sv);
	const auto marks = book.marks(path);
	assert_equal(true,
	             marks.size() == 1 && marks.front().signature == refs[0].signature &&
	                 marks.front().rejected.size() == 2 && marks.front().person == book.people()->named("Alex")->id &&
	                 marks.front().hidden == core::case_insensitive_path_identity,
	             "picture identity follows the platform's path comparison"sv);
}

static void should_backfill_face_signatures_without_choosing_between_conflicting_marks()
{
	using enum faces::face_action;
	face_database db;
	assert_equal(true, db.open(test_temps.next_path(".db")), "the database opens"sv);
	faces::face_book book;
	size_t writes = 0;
	book.persist_to([&](const faces::book_write& write)
	{
		++writes;
		assert_equal(true, db.write(write), "the bindings and decisions commit together"sv);
	});
	const auto ann = book.create("Ann");
	const auto tom = book.create("Tom");
	const rectd box{0.1, 0.2, 0.3, 0.4};
	faces::face_ref a{test_temps.next_path(".jpg"), box, 0}, b{test_temps.next_path(".jpg"), box, 0};
	edit_faces(book, std::array{a}, assign, ann);
	edit_faces(book, std::array{b}, assign, tom);
	edit_faces(book, std::array{a}, hide);
	const auto key = test_signature(5);
	a.signature = b.signature = key;
	std::array candidates{std::pair{a, book.marks(a.path).front()}, std::pair{b, book.marks(b.path).front()}};
	const auto before = book.generation();
	size_t omitted = 0;
	book.to_csv(&omitted);
	assert_equal(true,
	             omitted == 2 && !book.bind_signatures(candidates, before) && book.generation() == before &&
	                 !book.signature_mark(key),
	             "two different local confirmations stay local"sv);
	b.signature.reset();
	edit_faces(book, std::array{b}, reject, tom);
	b.signature = key;
	candidates[1] = {b, book.marks(b.path).front()};
	assert_equal(false, book.bind_signatures(candidates, before), "a stale migration cannot write"sv);
	const auto written = writes;
	assert_equal(true, book.bind_signatures(candidates, book.generation()) && writes == written + 1,
	             "a compatible confirmation, hide and rejection combine in one transaction"sv);
	faces::face_book disk;
	assert_equal(true, db.load(disk), "load the backfilled book"sv);
	const auto exact = disk.signature_mark(key);
	assert_equal(true,
	             exact && exact->hidden && exact->person == ann && exact->rejected == std::vector{tom} &&
	                 disk.unportable_count() == 0 && disk.mark_count() == 1 &&
	                 disk.marks(b.path).front().signature == key && disk.marks(b.path).front().person == ann,
	             "both pictures link to the one complete decision after reopening"sv);
	const auto edit = edit_faces(book, std::array{b}, assign, tom);
	assert_equal(true,
	             edit && book.marks(a.path).front().person == tom && !book.marks(a.path).front().hidden &&
	                 book.undo(*edit) && book.marks(a.path).front().person == ann,
	             "the other picture sees the current exact decision, including Undo"sv);
	book.clear();
	assert_equal(true, !book.mark_count() && db.load(disk) && !disk.mark_count() && disk.all_marks().empty(),
	             "forget clears both kinds of evidence and links"sv);
}

static void should_undo_a_face_placement_as_one_book_edit()
{
	face_database db;
	assert_equal(true, db.open(test_temps.next_path(".db")), "the database opens"sv);
	faces::face_book book;
	size_t writes = 0;
	book.persist_to([&](const faces::book_write& w)
	{
		++writes;
		assert_equal(true, db.write(w), "edit and inverse each commit"sv);
	});
	const auto path = test_temps.next_path(".jpg");
	const faces::face_ref left{path, {0.1, 0.1, 0.2, 0.2}, 0, test_signature(1)};
	const faces::face_ref right{path, {0.6, 0.1, 0.2, 0.2}, 1, test_signature(2)};
	const auto ann = book.create("Ann");
	edit_faces(book, std::array{left}, faces::face_action::assign, ann);
	edit_faces(book, std::array{left}, faces::face_action::hide);
	edit_faces(book, std::array{right}, faces::face_action::reject, ann);
	const auto before = book.to_csv();
	const std::array batches{faces::placement_batch{0, "Bea", {left}}, faces::placement_batch{ann, {}, {right}}};
	const auto started = book.generation();
	const auto written = writes;
	const auto placed = book.place(batches, faces::face_action::assign, started);
	assert_equal(true,
	             placed && placed->people.size() == 2 && book.people()->named("Bea") && writes == written + 1 &&
	                 book.generation() == started + 1,
	             "creating Bea, moving a hidden face, and overriding a rejection are one edit"sv);
	if (!placed) return;
	assert_equal(true, book.undo(*placed) && !book.people()->named("Bea"), "undo removes the new person"sv);
	faces::face_book disk;
	assert_equal(true, db.load(disk) && disk.to_csv() == before && book.to_csv() == before && writes == written + 2,
	             "undo restores all touched evidence in memory and on disk"sv);
	assert_equal(false, book.place(batches, faces::face_action::assign, started).has_value(),
	             "a stale preview cannot recreate Bea"sv);
	const auto again = book.place(batches, faces::face_action::assign, book.generation());
	assert_equal(true, again.has_value(), "a fresh preview can move again"sv);
	if (!again) return;
	const faces::face_ref later{test_temps.next_path(".jpg"), left.box, 0};
	edit_faces(book, std::array{later}, faces::face_action::assign, again->people.front());
	assert_equal(false, book.can_undo(*again) || book.undo(*again), "old inverse cannot delete a person now in use"sv);
}

static void should_combine_face_people_and_undo_as_one_database_edit()
{
	using enum faces::face_action;
	face_database db;
	const auto db_path = test_temps.next_path(".db");
	assert_equal(true, db.open(db_path), "open the book"sv);
	faces::face_book book;
	size_t writes = 0;
	book.persist_to([&](const faces::book_write& w)
	{
		++writes;
		assert_equal(true, db.write(w), "the whole change commits"sv);
	});
	const auto source = book.create("Alex S");
	const auto target = book.create("Alex Smith");
	const auto tom = book.create("Tom");
	const auto zac = book.create("Zac");
	book.combine("A.S.", "Alex S", book.generation());
	book.combine("Smith", "Alex Smith", book.generation());
	const auto path = test_temps.next_path(".jpg");
	const faces::face_ref left{path, {0.1, 0.1, 0.2, 0.2}, 0, test_signature(1)};
	const faces::face_ref right{path, {0.6, 0.1, 0.2, 0.2}, 1, test_signature(2)};
	const faces::face_ref unknown{test_temps.next_path(".jpg"), left.box, 0, test_signature(3)};
	for (const auto id : {tom, zac}) edit_faces(book, std::array{left, right, unknown}, reject, id);
	edit_faces(book, std::array{right, unknown}, reject, source);
	edit_faces(book, std::array{left, unknown}, reject, target);
	edit_faces(book, std::array{left}, assign, source);
	edit_faces(book, std::array{right}, assign, target);
	edit_faces(book, std::array{left, unknown}, hide);

	const auto before = book.to_csv();
	const auto started = book.generation();
	const auto written = writes;
	const auto edit = book.combine("a.s.", " smITH ", started);
	assert_equal(true, edit && book.generation() == started + 1 && writes == written + 1,
	             "one change, even through two former names"sv);
	if (!edit) return;
	const auto people = book.people();
	assert_equal(true,
	             !people->person(source) && people->canonical_name("Alex S") == "Alex Smith" &&
	                 people->canonical_name(" a.s. ") == "Alex Smith" && book.create("Alex S") == target &&
	                 people->canonical_name("Not kept") == "Not kept",
	             "target keeps its ID and spelling; former names resolve without another person"sv);
	const auto pair = book.marks(path);
	const auto other = book.marks(unknown.path);
	assert_equal(true,
	             pair.size() == 2 && pair[0].person == target && pair[1].person == target && pair[0].hidden &&
	                 !pair[1].hidden && pair[0].rejected == std::vector{tom, zac} &&
	                 pair[1].rejected == std::vector{tom, zac} && other.size() == 1 && other[0].hidden &&
	                 !other[0].person && other[0].rejected == std::vector{target, tom, zac},
	             "explicit confirmations take precedence in either direction; all other rejections and hiding stay"sv);
	faces::face_book disk;
	assert_equal(true, db.open(db_path) && db.load(disk) && disk.to_csv() == book.to_csv(),
	             "combined marks and aliases survive reopening"sv);
	assert_equal(false, book.combine("A.S.", "Alex Smith", book.generation()).has_value(),
	             "an alias and its owner are already one person"sv);
	assert_equal(true,
	             book.undo(*edit) && writes == written + 2 && book.to_csv() == before && db.load(disk) &&
	                 disk.to_csv() == before,
	             "one undo restores both IDs, aliases, hidden flags and every original rejection"sv);
	assert_equal(false, book.combine("Alex S", "Alex Smith", started).has_value(), "refuse a stale preview"sv);
}

static void should_combine_file_only_face_names()
{
	faces::face_book book;
	const auto empty = book.to_csv();
	const auto first = book.combine("A.", "Alex", book.generation());
	assert_equal(true, first && book.people()->people.size() == 1 && book.people()->canonical_name("a.") == "Alex",
	             "two names from files materialize just the retained person"sv);
	assert_equal(true, first && book.undo(*first) && book.to_csv() == empty, "undo restores a file-only book"sv);
	const auto old = book.create("A.");
	const auto next = book.combine("A.", "Alex", book.generation());
	const auto named = book.people()->named("Alex");
	assert_equal(true, next && named && named->id != old && !book.people()->person(old),
	             "a file-only target receives its own identity"sv);
	if (!next || !named) return;
	const auto target = named->id;
	const faces::face_ref ref{test_temps.next_path(".jpg"), {0.1, 0.2, 0.3, 0.4}, 0};
	const std::array assignment{faces::placement_batch{0, "a.", {ref}}};
	const auto placed = book.place(assignment, faces::face_action::assign, book.generation());
	assert_equal(true, placed && placed->people == std::vector{target} && !book.undo(*next),
	             "assignment resolves the former name; intervening evidence expires combine undo"sv);
}

static void should_restore_combined_face_names_from_csv()
{
	faces::face_book kept;
	kept.combine("Ann, Jr.", "Anne", kept.generation());
	kept.combine("Annie", "Anne", kept.generation());
	const std::array confirmed{
		faces::face_ref{test_temps.next_path(".jpg"), {0.1, 0.2, 0.3, 0.4}, 0, test_signature(1)}};
	edit_faces(kept, confirmed, faces::face_action::assign, kept.create("Annie"));
	edit_faces(kept, confirmed, faces::face_action::hide);
	const auto csv = kept.to_csv();
	faces::face_book fresh;
	assert_equal(true, fresh.import_csv(csv).ok && fresh.to_csv() == csv, "combined file names round trip"sv);

	faces::face_book local;
	local.create("Unrelated"); // Imported IDs have no meaning here.
	const auto existing = local.create("Ann, Jr.");
	const auto merged = local.import_csv(csv);
	const auto after = local.generation();
	const auto joined = local.signature_mark(*confirmed.front().signature);
	assert_equal(true,
	             merged.ok && merged.people_joined == 1 && !merged.people_added && joined &&
	                 joined->person == existing && joined->hidden && local.create("Anne") == existing &&
	                 local.people()->canonical_name("Annie") == "Ann, Jr." && local.import_csv(csv).ok &&
	                 local.generation() == after,
	             "join even by an imported former name, keeping local ID and spelling; reimport is a no-op"sv);

	// In a v2 backup all three names had separate IDs. Two rejections now name one person; the confirmation
	// conflicts only after remapping, even though this picture is new to the local profile.
	const auto path = test_temps.next_path(".jpg");
	const auto v2 = legacy_face_header(2) +
	                "person,1,\"Ann, Jr.\",,,,,,,,,\r\nperson,2,Anne,,,,,,,,,\r\n"
	                "person,3,Annie,,,,,,,,,\r\nperson,4,Zac,,,,,,,,,\r\n" +
	                std::format("face,,,\"{}\",0.1,0.2,0.3,0.4,0,1,1,2 3 4\r\n", path.str());
	const auto imported = local.import_csv(v2);
	const auto marks = local.marks(path);
	const auto* local_zac = local.people()->named("Zac");
	assert_equal(true,
	             imported.ok && local_zac && marks.size() == 1 && marks[0].hidden && !marks[0].person &&
	                 marks[0].rejected == std::vector{existing, local_zac->id},
	             "older backups resolve aliases and preserve the union of rejections"sv);

	faces::face_book ambiguous;
	ambiguous.create("Anne");
	ambiguous.create("Annie");
	const auto before = ambiguous.to_csv();
	const auto generation = ambiguous.generation();
	assert_equal(true,
	             !ambiguous.import_csv(csv).ok && ambiguous.to_csv() == before && ambiguous.generation() == generation,
	             "an import cannot silently combine two different existing people"sv);
}

static void should_migrate_an_older_face_database_without_losing_evidence()
{
	for (const auto version : {2, 3})
	{
		const auto path = test_temps.next_path(".db");
		sqlite3* raw = nullptr;
		assert_equal(SQLITE_OK, sqlite3_open(path.str().c_str(), &raw), "create original schema"sv);
		const auto folder = core::windows_path_semantics ? "C:\\Photos"sv : "/photos"sv;
		const auto fixture = std::format(
			"CREATE TABLE face_people (id INTEGER PRIMARY KEY CHECK(id BETWEEN 1 AND 4294967295), "
			"name TEXT NOT NULL CHECK(length(name) > 0));"
			"CREATE TABLE face_marks (id INTEGER PRIMARY KEY, folder TEXT NOT NULL, name TEXT NOT NULL, "
			"x REAL NOT NULL, y REAL NOT NULL, w REAL NOT NULL, h REAL NOT NULL, position INTEGER NOT NULL, "
			"person INTEGER REFERENCES face_people(id), hidden INTEGER NOT NULL CHECK(hidden IN (0,1)));"
			"CREATE TABLE face_rejections (mark INTEGER NOT NULL REFERENCES face_marks(id) ON DELETE CASCADE, "
			"person INTEGER NOT NULL REFERENCES face_people(id), PRIMARY KEY(mark,person)) WITHOUT ROWID;"
			"INSERT INTO face_people VALUES (7,'Ann'),(15,'Zac'),(27,'Tom');"
			"INSERT INTO face_marks VALUES(5,'{}','one.jpg',0.1,0.2,0.3,0.4,2,7,1);"
			"INSERT INTO face_rejections VALUES(5,15),(5,27); PRAGMA user_version={};",
			folder, version);
		assert_equal(SQLITE_OK, sqlite3_exec(raw, fixture.c_str(), nullptr, nullptr, nullptr),
		             "populated older book"sv);
		if (version == 3)
			assert_equal(
				SQLITE_OK,
				sqlite3_exec(raw,
			                 "CREATE TABLE face_aliases (name TEXT PRIMARY KEY CHECK(length(name) > 0), "
			                 "person INTEGER NOT NULL REFERENCES face_people(id) ON DELETE CASCADE) WITHOUT ROWID; "
			                 "INSERT INTO face_aliases VALUES ('A.',7)",
			                 nullptr, nullptr, nullptr),
				"original former name"sv);
		sqlite3_close(raw);
		face_database db;
		faces::face_book book;
		assert_equal(true, db.open(path) && db.load(book), "open and migrate without replacing existing tables"sv);
		const auto marks = book.marks(core::file_path(core::folder_path(folder), "one.jpg"));
		assert_equal(true,
		             book.people()->people.size() == 3 && marks.size() == 1 && marks[0].person == 7 &&
		                 marks[0].hidden && marks[0].position == 2 && marks[0].rejected == std::vector{15u, 27u} &&
		                 book.unportable_count() == 1 && (version == 2 || book.people()->canonical_name("A.") == "Ann"),
		             "the migration preserves the user's original IDs, positive and negative evidence"sv);
		book.persist_to([&](const faces::book_write& w)
		{ assert_equal(true, db.write(w), "write to migrated book"sv); });
		const auto edit = book.combine("Annie", "Ann", book.generation());
		faces::face_book disk;
		assert_equal(true, edit && db.open(path) && db.load(disk) && disk.to_csv() == book.to_csv(),
		             "migrated book can persist a former name and reopen"sv);
	}
}

static void should_refuse_an_incompatible_face_database()
{
	for (const int version : {0, 1, 2, 3, 4, 99})
	{
		const auto path = test_temps.next_path(".db");
		sqlite3* raw = nullptr;
		assert_equal(SQLITE_OK, sqlite3_open(path.str().c_str(), &raw), "create fixture"sv);
		const auto sql = std::format("CREATE TABLE face_groups(id INTEGER PRIMARY KEY); "
		                             "INSERT INTO face_groups VALUES (42); PRAGMA user_version={}",
		                             version);
		assert_equal(SQLITE_OK, sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), "incompatible schema"sv);
		sqlite3_close(raw);
		face_database db;
		assert_equal(false, db.open(path) || db.is_open(), "an incompatible file is not opened for writing"sv);
		assert_equal(SQLITE_OK, sqlite3_open(path.str().c_str(), &raw), "read untouched fixture"sv);
		sqlite3_stmt* read = nullptr;
		assert_equal(SQLITE_OK,
		             sqlite3_prepare_v2(raw,
		                                "SELECT id,(SELECT user_version FROM pragma_user_version),"
		                                "(SELECT COUNT(*) FROM sqlite_master WHERE type='table') FROM face_groups",
		                                -1, &read, nullptr),
		             "read original schema"sv);
		assert_equal(SQLITE_ROW, sqlite3_step(read), "original row remains"sv);
		assert_equal(true,
		             sqlite3_column_int(read, 0) == 42 && sqlite3_column_int(read, 1) == version &&
		                 sqlite3_column_int(read, 2) == 1,
		             "no data, version or tables changed"sv);
		sqlite3_finalize(read);
		sqlite3_close(raw);
	}
}

static void should_keep_writing_after_a_face_database_commit_fails()
{
	const auto path = test_temps.next_path(".db");
	face_database db;
	assert_equal(true, db.open(path), "the face database opens"sv);

	faces::book_write change;
	change.people.push_back({1, "Original"});
	assert_equal(true, db.write(change), "the initial change commits"sv);

	// A rollback-journal reader allows BEGIN IMMEDIATE and the update, but blocks COMMIT.
	{
		sqlite3* handle = nullptr;
		const auto opened = sqlite3_open(path.str().c_str(), &handle);
		const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> reader(handle, sqlite3_close_v2);
		assert_equal(SQLITE_OK, opened, "second connection opens"sv);
		assert_equal(SQLITE_OK,
		             sqlite3_exec(reader.get(), "BEGIN; SELECT * FROM face_people;", nullptr, nullptr, nullptr),
		             "second connection holds a read lock"sv);
		change.people.front().name = "Uncommitted";
		assert_equal(false, db.write(change), "the write cannot commit while the reader holds its lock"sv);
	}

	faces::face_book book;
	assert_equal(true, db.load(book), "the writer can still read"sv);
	assert_equal("Original"sv, std::string_view(book.people()->people.front().name), "failed change rolled back"sv);
	change.people.front().name = "Committed";
	assert_equal(true, db.write(change), "the same connection can commit a later change"sv);
	assert_equal(true, db.load(book), "the later change can be read"sv);
	assert_equal("Committed"sv, std::string_view(book.people()->people.front().name), "later change persisted"sv);
}

// docs/faces.md#64-the-face-database: two windows on one profile - the first writes the face book, the second only
// reads it, sees what the first changes, and writes it once the first has closed, as the first left it.
static void should_let_one_window_write_the_face_book_and_hand_it_over()
{
	const auto folder = test_temps.next_folder("faces-two-windows");
	const auto path = folder.combine_file(face_database::file_name);
	deferred_async_strategy first_async, second_async;
	const location_cache locations;
	index_state first_index(first_async, locations), second_index(second_async, locations);
	auto first = std::make_unique<database>(first_index);
	database second(second_index);
	first->open(folder, "first");
	second.open(folder, "second");
	first->open_faces(path);
	second.open_faces(path);
	auto& writer = first_index.faces();
	auto& reader = second_index.faces();
	assert_equal(true,
	             writer.book_state() == faces::book_keeping::kept &&
	                 reader.book_state() == faces::book_keeping::shared && reader.book().is_read_only() &&
	                 !reader.can_change_book() && reader.book_needs_polling(),
	             "the first window writes the face book and the second only reads it"sv);

	writer.book().create("Ann");
	first->perform_writes();
	second.poll_faces();
	assert_equal(true, reader.book().people()->named("Ann") != nullptr, "the reader sees what the writer changed"sv);
	assert_equal(0u, reader.book().create("Bob"), "and cannot change it"sv);

	first.reset();
	second.poll_faces();
	assert_equal(true,
	             reader.book_state() == faces::book_keeping::kept && !reader.book().is_read_only() &&
	                 reader.book().people()->named("Ann") != nullptr,
	             "once the writer closes, the reader takes the book over as it was left"sv);
	assert_equal(true, reader.book().create("Bob") != 0, "and can change it"sv);
	second.perform_writes();
	face_database check;
	faces::face_book disk;
	assert_equal(true,
	             check.open(path, face_database::access::read) && check.load(disk) && disk.people()->named("Ann") &&
	                 disk.people()->named("Bob"),
	             "which it then writes"sv);
}

// docs/faces.md#64-the-face-database: a change that cannot be written leaves the book to be written whole as soon as it
// can be, and says meanwhile that it is not saved.
static void should_write_the_face_book_whole_after_a_write_fails()
{
	const auto folder = test_temps.next_folder("faces-unsaved");
	const auto path = folder.combine_file(face_database::file_name);
	deferred_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	database db(index);
	db.open(folder, "index");
	db.open_faces(path);
	auto& faces = index.faces();
	faces.book().create("Ann");
	db.perform_writes();
	{
		// A rollback-journal reader allows the write's update, but blocks its COMMIT.
		sqlite3* handle = nullptr;
		const auto opened = sqlite3_open(path.str().c_str(), &handle);
		const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> reader(handle, sqlite3_close_v2);
		assert_equal(SQLITE_OK, opened, "second connection opens"sv);
		assert_equal(SQLITE_OK,
		             sqlite3_exec(reader.get(), "BEGIN; SELECT * FROM face_people;", nullptr, nullptr, nullptr),
		             "second connection holds a read lock"sv);
		faces.book().create("Bob");
		db.perform_writes();
		assert_equal(true, faces.book_state() == faces::book_keeping::unsaved && faces.book_needs_polling(),
		             "the change waits, and the book says it is not saved yet"sv);
	}
	db.poll_faces();
	face_database check;
	faces::face_book disk;
	assert_equal(true,
	             faces.book_state() == faces::book_keeping::kept && check.open(path, face_database::access::read) &&
	                 check.load(disk) && disk.people()->named("Ann") && disk.people()->named("Bob"),
	             "the book is written whole once it can be"sv);
}

// docs/faces.md#64-the-face-database: a book another window writes refuses every change in this one.
static void should_refuse_changes_to_a_read_only_face_book()
{
	faces::face_book book;
	const auto ann = book.create("Ann");
	const faces::face_ref ref{test_temps.next_path(".jpg"), {0.1, 0.1, 0.2, 0.2}, 0, test_signature(1)};
	const auto csv = book.to_csv();
	book.set_read_only(true);
	const auto before = book.generation();
	book.clear();
	assert_equal(true,
	             book.create("Bob") == 0 && !book.rename(ann, "Anne") &&
	                 !edit_faces(book, std::array{ref}, faces::face_action::assign, ann) && !book.import_csv(csv).ok &&
	                 book.generation() == before && book.people()->named("Ann") != nullptr,
	             "nothing changes, not even a forget"sv);
}

// docs/faces.md#61-named-people: renaming someone pictures also name keeps the old name as a former name, so those
// pictures are still theirs.
static void should_keep_a_former_name_when_renaming_someone_files_name()
{
	faces::face_book book;
	const auto ann = book.create("Ann Lee");
	const auto bob = book.create("Bob");
	assert_equal(true, book.rename(ann, "Ann Smith", true) && book.rename(bob, "Robert"), "both are renamed"sv);
	const auto roster = book.people();
	assert_equal(true,
	             roster->aliases.contains("Ann Lee") && roster->canonical_name("ann lee") == "Ann Smith" &&
	                 roster->named("Ann Lee") && roster->named("Ann Lee")->id == ann,
	             "the old name still finds the person"sv);
	assert_equal(false, roster->aliases.contains("Bob"), "a rename no file needs keeps no former name"sv);
}

// A face of someone like another: mostly along `axis`, leaning along `towards`. Two people leaning apart from one axis
// resemble each other past `confident`, as young brothers and sisters do, while each resembles their own faces far
// more.
static faces::face_vector leaning_vector(const int axis, const int towards, const float lean, const int seed)
{
	auto v = axis_vector(axis, 0.01f, seed);
	v[towards] += lean;
	auto length = 0.0f;
	for (const auto x : v) length += x * x;
	for (auto& x : v) x /= std::sqrt(length);
	return v;
}

static std::vector<faces::named_region> no_regions(core::file_path)
{
	return {};
}

namespace
{
	struct recognition_fixture
	{
		faces::face_store_ptr store = std::make_shared<faces::face_store>();
		faces::face_book book;
		faces::marks_by_picture marks;
		const rectd left{0.1, 0.1, 0.2, 0.2}, right{0.6, 0.1, 0.2, 0.2};

		static core::file_path path(const std::string_view name)
		{
			return core::file_path(core::folder_path("C:\\pictures"), name);
		}
		uint32_t add(const std::string_view name, std::vector<faces::found_face> found)
		{
			const auto first = store->face_count();
			faces::face_record record;
			record.model = faces::model_id();
			record.faces = std::move(found);
			store->add(path(name), record);
			return first;
		}
		faces::overview_ptr compare(const faces::regions_of& read = no_regions,
		                            const faces::tuning& t = faces::defaults(),
		                            const faces::parallel_for& parallel = {})
		{
			book.load(*book.people(), marks);
			store->publish();
			return faces::make_overview(store, read, book, [] { return false; }, t, parallel);
		}
	};

	// Several threads each taking the next piece nobody has, as the app lends the overview its comparisons: for tests
	// that must get the same answer on one thread as on several.
	faces::parallel_for threads_for(const int threads)
	{
		return [threads](const size_t count, const std::function<void(size_t, size_t)>& body)
		{
			const auto piece = std::max<size_t>(1, count / (static_cast<size_t>(threads) * 8));
			std::atomic_size_t next = 0;
			std::vector<std::jthread> pool;
			for (auto t = 0; t < threads; ++t)
				pool.emplace_back([&]
				{
					for (auto i = next.fetch_add(1); i * piece < count; i = next.fetch_add(1))
						body(i * piece, std::min(count, (i + 1) * piece));
				});
		};
	}
}

static void should_distinguish_a_named_face_from_an_offer_and_a_contradicted_name()
{
	recognition_fixture fixture;
	const auto zac = fixture.book.create("Zac");
	const auto jana = fixture.book.create("Jana");
	const auto& l = fixture.left;
	const auto& r = fixture.right;
	const auto face = face_like(axis_vector(1, 0.01f, 1), l);
	const auto other = face_like(axis_vector(3, 0.01f, 1), r);
	const auto named = fixture.add("named.jpg", {face});
	const auto offered = fixture.add("offered.jpg", {face});
	const auto corrected = fixture.add("corrected.jpg", {other});
	fixture.add("two.jpg", {face, other});
	auto no_hash = face;
	no_hash.hash = {};
	const auto missing = fixture.add("missing.jpg", {no_hash});
	const auto hidden = fixture.add("hidden.jpg", {face});
	fixture.marks[fixture.path("corrected.jpg")] = {{r, 0, jana}};
	fixture.marks[fixture.path("two.jpg")] = {{r, 1, jana}};
	fixture.marks[fixture.path("hidden.jpg")] = {{l, 0, 0, true}};

	const auto o = fixture.compare([&](const core::file_path path) -> faces::picture_evidence
	{
		if (path.name() == "two.jpg") return {{{"Zac", l}, {"Zac", r}}, 2001, core::cache("Zac")};
		if (path.name() == "corrected.jpg") return {{{"Zac", r}}, 2001, core::cache("Zac")};
		if (path.name() == "named.jpg") return {{{"Zac", l}}, 2001, core::cache("Zac")};
		return {{}, 0};
	});
	const auto* p = o->find_person("zac");
	assert_equal(true, p && p->id == zac && p->confirmed.size() == 2 && p->suggested.size() == 1,
	             "a single person for file and book names; an offer is separate"sv);
	assert_equal(true, o->member_of_face(named)->kind == faces::member_kind::named, "file is the evidence"sv);
	assert_equal(true, o->person_of_face(corrected)->id == jana, "the local correction takes precedence"sv);
	assert_equal(true, o->file_name_contradicted(fixture.path("corrected.jpg"), "ZAC"),
	             "the file name is superseded"sv);
	assert_equal(false, o->file_name_contradicted(fixture.path("two.jpg"), "Zac"), "the second region still counts"sv);
	assert_equal(true, o->unknown == std::vector<uint32_t>{missing} && o->member_of_face(offered),
	             "Unknown keeps the missing hash, not the face offered to Zac"sv);
	assert_equal(true, o->hidden == std::vector<uint32_t>{hidden}, "only the explicitly hidden face"sv);
	const auto overlay = faces::faces_on_picture(*fixture.store, o.get(), fixture.book, fixture.path("offered.jpg"));
	assert_equal(true, overlay.size() == 1 && overlay.front().offered && overlay.front().name == "Zac",
	             "the photo can label an offer tentatively, never as a confirmation"sv);
	assert_equal(true,
	             faces::faces_on_picture(*fixture.store, o.get(), fixture.book, fixture.path("hidden.jpg")).empty(),
	             "no overlay on the hidden face"sv);
}

static void should_keep_faces_below_half_similarity_in_unknown()
{
	recognition_fixture f;
	const auto zac = f.book.create("Zac");
	faces::found_face sample;
	sample.box = f.left;
	// Synthetic quantized hashes with known dot products, independent of detector confidence.
	sample.hash = {};
	sample.hash.scale = 1.0f;
	sample.hash.values[0] = 1;
	f.add("confirmed.jpg", {sample});
	f.marks[f.path("confirmed.jpg")] = {{f.left, 0, zac}};
	sample.hash.scale = 0.01f;
	sample.hash.values[0] = 49;
	const auto below = f.add("below.jpg", {sample});
	sample.hash.values[0] = 50;
	const auto at = f.add("at.jpg", {sample});
	const auto overview = f.compare();
	const auto* p = overview->find_person("Zac");
	assert_equal(true,
	             p && p->suggested.size() == 1 && p->suggested.front().face == at &&
	                 overview->unknown == std::vector{below},
	             "49% is unknown; exactly 50% can be offered and leaves the inbox");
}

static void should_group_similar_unknown_faces_for_selection()
{
	recognition_fixture fixture;
	const auto a = [&](const int i)
	{
		return face_like(axis_vector(1, 0.04f, i), fixture.left);
	};
	const auto b = [&](const int i)
	{
		return face_like(axis_vector(31, 0.04f, i), fixture.left);
	};
	const auto a1 = fixture.add("a1.jpg", {a(1)});
	const auto b1 = fixture.add("b1.jpg", {b(2)});
	const auto missing = fixture.add("no-hash.jpg", {faces::found_face{}});
	const auto a2 = fixture.add("a2.jpg", {a(3)});
	const auto b2 = fixture.add("b2.jpg", {b(4)});
	const auto a3 = fixture.add("a3.jpg", {a(5)});
	const auto overview = fixture.compare();
	assert_equal(true, overview->people.empty() && overview->unknown.size() == 6,
	             "visual ordering doesn't create people or take any face out of Unknown");

	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto view = std::make_shared<faces_view>(state, nullptr);
	auto model = std::make_shared<faces_view_model>();
	model->store = fixture.store;
	model->overview = overview;
	view->model_arrived_for_tests(model);
	std::vector<uint32_t> shown;
	for (const auto& c : view->cells()) shown.push_back(c.shown.face.value());
	assert_equal(true, shown == std::vector{a1, a2, a3, b1, b2, missing},
	             "lookalikes from different photos are adjacent, and the face without a hash is still shown");
	view->click_cell(0, {});
	view->click_cell(2, {.shift = true});
	assert_equal(true, view->selected_cells() == std::vector{0, 1, 2}, "the adjacent lookalikes can be range selected");
	view->deactivate();
}

// docs/faces.md#72-suggestions: a pass shares its comparisons among the threads it is lent, and gets the answer one
// thread gets - the same people, offers and scores, and Unknown in the same order.
static void should_make_the_same_overview_on_one_thread_as_on_several()
{
	recognition_fixture f;
	std::vector<uint32_t> ids;
	for (auto p = 0; p < 6; ++p) ids.push_back(f.book.create(std::format("Person {}", p)));
	for (auto k = 0; k < 400; ++k)
	{
		const auto name = std::format("p{}.jpg", k);
		// One of the people, or one of the strangers who recur, beside another stranger, some of them unclear.
		const auto axis = k % 3 == 0 ? 1 + (k / 3) % 6 * 7 : 50 + k % 40;
		f.add(name, {face_like(axis_vector(axis, 0.06f, k), f.left),
		             face_like(axis_vector(90 + k % 30, 0.06f, k + 1000), f.right, k % 5 != 0)});
		if (k < 36 && k % 3 == 0) f.marks[f.path(name)] = {{f.left, 0, ids[(k / 3) % 6]}};
	}
	const auto one = f.compare();
	const auto several = f.compare(no_regions, faces::defaults(), threads_for(4));
	const auto same_members = [](const std::vector<faces::person_member>& a, const std::vector<faces::person_member>& b)
	{
		return std::ranges::equal(a, b, [](const auto& x, const auto& y)
		{ return x.face == y.face && x.score == y.score && x.kind == y.kind; });
	};
	auto same = one->people.size() == several->people.size();
	size_t offers = 0;
	for (size_t p = 0; same && p < one->people.size(); ++p)
	{
		same = one->people[p].name == several->people[p].name &&
		       same_members(one->people[p].confirmed, several->people[p].confirmed) &&
		       same_members(one->people[p].suggested, several->people[p].suggested);
		offers += one->people[p].suggested.size();
	}
	assert_equal(true, offers > 0 && one->unknown_runs.size() > 1,
	             "the pass has offers to make and lookalikes to order");
	assert_equal(true, same, "the same people, offers and scores");
	assert_equal(true,
	             one->unknown_for_review() == several->unknown_for_review() &&
	                 one->unknown_runs == several->unknown_runs && one->unclear_from == several->unclear_from,
	             "and Unknown in the same order");
}

// docs/faces.md#91-people: Unknown in review order - the largest run of lookalikes first, then the single faces, then
// those too small or unclear to carry an identity.
static void should_list_unknown_faces_as_runs_of_lookalikes_largest_first()
{
	recognition_fixture f;
	const auto a = [&](const int i)
	{
		return face_like(axis_vector(1, 0.04f, i), f.left);
	};
	const auto b = [&](const int i)
	{
		return face_like(axis_vector(31, 0.04f, i), f.left);
	};
	const auto b1 = f.add("b1.jpg", {b(1)});
	const auto single = f.add("single.jpg", {face_like(axis_vector(61, 0.04f, 2), f.left)});
	const auto unclear = f.add("unclear.jpg", {face_like(axis_vector(91, 0.04f, 3), f.left, false)});
	const auto a1 = f.add("a1.jpg", {a(4)});
	const auto b2 = f.add("b2.jpg", {b(5)});
	const auto a2 = f.add("a2.jpg", {a(6)});
	const auto a3 = f.add("a3.jpg", {a(7)});
	const auto o = f.compare();
	assert_equal(true, o->unknown_for_review() == std::vector{a1, a2, a3, b1, b2, single, unclear},
	             "the three lookalikes, the two, the clear single face, then the unclear one");
	assert_equal(true, o->unknown_runs == std::vector<uint32_t>{3, 2} && o->unclear_from == 6,
	             "with each run's length and where the unclear faces start");
}

// docs/faces.md#91-people: only faces clear enough to carry an identity form runs. Unclear faces - blurred or small,
// a pet or a toy - are listed with the small or unclear ones however alike they look, where a run of them once led
// Unknown as its largest.
static void should_keep_unclear_lookalikes_out_of_runs()
{
	recognition_fixture f;
	const auto clear = [&](const int i)
	{
		return face_like(axis_vector(1, 0.04f, i), f.left);
	};
	const auto unclear = [&](const int i)
	{
		return face_like(axis_vector(61, 0.04f, i), f.left, false);
	};
	const auto u1 = f.add("u1.jpg", {unclear(1)});
	const auto a1 = f.add("a1.jpg", {clear(2)});
	const auto u2 = f.add("u2.jpg", {unclear(3)});
	const auto u3 = f.add("u3.jpg", {unclear(4)});
	const auto a2 = f.add("a2.jpg", {clear(5)});
	const auto single = f.add("single.jpg", {face_like(axis_vector(31, 0.04f, 6), f.left)});
	const auto o = f.compare();
	assert_equal(true, o->unknown_runs == std::vector<uint32_t>{2} && o->unclear_from == 3,
	             "the two clear lookalikes are the only run, and the single clear face follows it");
	assert_equal(true, o->unknown_for_review() == std::vector{a1, a2, single, u1, u2, u3},
	             "three unclear lookalikes are listed with the unclear faces, not as a run ahead of the rest");
}

// docs/faces.md#91-people: a run's heading can name or hide every face under it, and the unique and the unclear faces'
// headings can hide theirs; Hide all hides just those faces, with an Undo, and Name all selects its run to be named.
// The scroll bar beside them is as wide as their widest word, not their widest name.
static void should_name_or_hide_a_whole_unknown_group_from_its_heading()
{
	recognition_fixture f;
	const auto clear = [&](const int i)
	{
		return face_like(axis_vector(1, 0.04f, i), f.left);
	};
	for (auto i = 1; i <= 3; ++i) f.add(std::format("a{}.jpg", i), {clear(i)});
	f.add("single.jpg", {face_like(axis_vector(31, 0.04f, 4), f.left)});
	f.add("u1.jpg", {face_like(axis_vector(61, 0.04f, 5), f.left, false)});
	f.add("u2.jpg", {face_like(axis_vector(91, 0.04f, 6), f.left, false)});
	const auto overview = f.compare();

	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto saved = setting.faces.groups_width;
	const core::scope_exit restore([&] { setting.faces.groups_width = saved; });
	setting.faces.groups_width = 200;
	const auto view = std::make_shared<faces_view>(state, nullptr);
	auto model = std::make_shared<faces_view_model>();
	model->store = f.store;
	model->overview = overview;
	view->model_arrived_for_tests(model);
	ui::test::fake_measure_context mc;
	mc.scale_metrics(1.0);
	view->layout(mc, {800, 240});

	using action = faces_view::heading_action;
	const auto buttons = [&](const size_t h)
	{
		std::vector<action> result;
		for (const auto& b : view->headings()[h].buttons) result.push_back(b.action);
		return result;
	};
	const auto& headings = view->headings();
	assert_equal(true,
	             headings.size() == 3 && headings[1].text == tt.faces_other.sv() &&
	                 headings[2].text == tt.faces_unclear.sv(),
	             "the run, then the unique faces, then the unclear ones, each under a heading");
	assert_equal(true,
	             buttons(0) == std::vector{action::name, action::hide} && buttons(1) == std::vector{action::hide} &&
	                 buttons(2) == std::vector{action::hide},
	             "a run can be named or hidden; the unique and unclear faces only hidden");
	assert_equal(true,
	             std::ranges::all_of(headings,
	                                 [](const faces_view::heading& h)
	{
		return std::ranges::all_of(h.buttons, [&h](const faces_view::heading_button& b)
		{ return !b.bounds.is_empty() && h.bounds.intersection(b.bounds) == b.bounds; });
	}),
	             "each button is laid out on its heading");

	auto& book = index.faces().book();
	view->press_heading_button(2, action::hide);
	const auto hidden = [&](const std::string_view name)
	{
		const auto marks = book.marks(f.path(name));
		return marks.size() == 1 && marks.front().hidden;
	};
	assert_equal(true, hidden("u1.jpg") && hidden("u2.jpg") && book.marks(f.path("single.jpg")).empty(),
	             "Hide all hides the faces under its heading, and no others");
	assert_equal(true, view->can_undo_placement(), "and leaves the receipt H leaves, with its Undo");
	view->undo_placement();
	assert_equal(true, book.marks(f.path("u1.jpg")).empty(), "which shows them again");

	view->press_heading_button(0, action::name);
	assert_equal(true, view->selected_cells() == std::vector{0, 1, 2}, "Name all selects the run it names");

	const auto rail = view->faces_scroller().scroll_bounds().width();
	assert_equal(true,
	             rail == ui::table_layout::widest_word(mc, tt.faces_unclear.sv()) + mc.padding1 * 2 &&
	                 rail < mc.measure_text(tt.faces_unclear.sv(), render::font_face::dialog,
	                                        render::text_style::single_line, 1000)
	                            .cx,
	             "the scroll bar is as wide as its widest word, not its widest name");
	view->deactivate();
}

// docs/faces.md#72-suggestions: a picture holding one face, whose metadata names one person and places nobody, offers
// that face to them, as a suggestion; two faces, two names, a region or a rejection offer nothing.
static void should_offer_the_one_face_of_a_picture_to_the_one_person_it_names()
{
	recognition_fixture f;
	const auto ann = f.book.create("Ann");
	const auto face = [&](const int axis)
	{
		return face_like(axis_vector(axis, 0.04f, axis), f.left);
	};
	faces::found_face beside;
	beside.box = f.right;
	const auto named = f.add("named.jpg", {face(1)});
	const auto two_faces = f.add("two-faces.jpg", {face(11), beside});
	const auto two_names = f.add("two-names.jpg", {face(21)});
	const auto rejected = f.add("rejected.jpg", {face(31)});
	const auto placed = f.add("placed.jpg", {face(41)});
	const auto bob = f.add("bob.jpg", {face(51)});
	f.marks[f.path("rejected.jpg")] = {{f.left, 0, 0, false, {ann}}};
	const auto read = [&](const core::file_path path) -> faces::picture_evidence
	{
		const auto names = [](std::vector<std::string> n)
		{
			return core::cache(core::combine(n));
		};
		if (path == f.path("named.jpg") || path == f.path("two-faces.jpg") || path == f.path("rejected.jpg"))
			return {{}, 0, names({"Ann"})};
		if (path == f.path("two-names.jpg")) return {{}, 0, names({"Ann", "Bob"})};
		if (path == f.path("placed.jpg")) return {{{"Ann", f.left}}, 0, names({"Ann"})};
		if (path == f.path("bob.jpg")) return {{}, 0, names({"Bob"})};
		return {{}};
	};
	const auto o = f.compare(read);
	const auto* a = o->find_person("Ann");
	assert_equal(true, a && a->suggested.size() == 1 && a->suggested.front().face == named,
	             "the picture's one face is offered to the one person it names");
	assert_equal(true,
	             a && a->confirmed.size() == 1 && a->confirmed.front().face == placed && a->confirmed_pictures == 1 &&
	                 a->pictures == 2,
	             "a region still places its face, and the offer is counted apart from it");
	assert_equal(true, !o->person_of_face(two_faces) && !o->person_of_face(two_names) && !o->person_of_face(rejected),
	             "not where the picture has two faces or names two people, nor to a person rejected for the face");
	const auto* b = o->find_person("Bob");
	assert_equal(true,
	             b && b->id == 0 && b->confirmed.empty() && b->suggested.size() == 1 &&
	                 b->suggested.front().face == bob,
	             "a person only files name is offered their face too");
}

// docs/faces.md#72-suggestions: a person's main portrait is a face clear enough to carry an identity, where they have
// one, before a sharper-measured face too small to.
static void should_show_a_clear_face_as_a_persons_portrait()
{
	recognition_fixture f;
	const auto zac = f.book.create("Zac");
	auto small = face_like(axis_vector(1, 0.04f, 1), f.left, false);
	small.frontal = 255;
	small.confidence = 100;
	auto clear = face_like(axis_vector(1, 0.04f, 2), f.left);
	clear.frontal = 160;
	clear.eye_distance = 30;
	f.add("small.jpg", {small});
	const auto big = f.add("big.jpg", {clear});
	f.marks[f.path("small.jpg")] = {{f.left, 0, zac}};
	f.marks[f.path("big.jpg")] = {{f.left, 0, zac}};
	const auto o = f.compare();
	const auto* p = o->find_person("Zac");
	assert_equal(true, p && p->samples.size() == 2 && p->samples.front() == big, "the clear face is the main portrait");
}

static void should_offer_only_clear_face_matches_using_positive_and_negative_evidence()
{
	recognition_fixture f;
	const auto& box = f.left;
	const auto amy = f.book.create("Amy");
	const auto ben = f.book.create("Ben");
	const auto face = [&](const float lean)
	{
		return face_like(leaning_vector(1, 2, lean, 1), box);
	};
	const auto a = f.add("a.jpg", {face(0.6f)});
	f.add("b.jpg", {face(-0.6f)});
	f.add("not-a.jpg", {face(0.15f)});
	const auto either = f.add("either.jpg", {face(0.0f)});
	const auto like_negative = f.add("like-negative.jpg", {face(0.16f)});
	const auto like_amy = f.add("yes.jpg", {face(0.65f), face(0.55f)});
	const auto eligible = f.add("rejected-for-a.jpg", {face(-0.6f)});
	f.marks[f.path("a.jpg")] = {{box, 0, amy}};
	f.marks[f.path("b.jpg")] = {{box, 0, ben}};
	f.marks[f.path("not-a.jpg")] = {{box, 0, 0, false, {amy}}};
	f.marks[f.path("rejected-for-a.jpg")] = {{box, 0, 0, false, {amy}}};
	const auto o = f.compare();
	assert_equal(true, o->member_of_face(a)->score == faces::not_compared, "the one anchor cannot score itself"sv);
	assert_equal(true, o->person_of_face(either) == nullptr, "runner-up margin rules out an ambiguous match"sv);
	assert_equal(true, o->person_of_face(like_negative) == nullptr, "a lookalike rejection outweighs this offer"sv);
	assert_equal(true, o->find_person("Amy")->suggested.size() == 1, "only one offer per photo and person"sv);
	assert_equal(true,
	             o->find_person("Amy")->suggested.front().face >= like_amy &&
	                 o->find_person("Amy")->suggested.front().face <= like_amy + 1,
	             "only the clear photo yields an offer for Amy"sv);
	assert_equal(true, o->person_of_face(eligible) && o->person_of_face(eligible)->id == ben,
	             "an exact No for Amy permits an offer for Ben"sv);
	assert_equal(true, o->hidden.empty(), "negative examples never hide another face"sv);
}

static void should_compare_and_show_different_looks_without_training_on_suggested_faces()
{
	recognition_fixture f;
	const auto cleo = f.book.create("Cleo");
	const auto& box = f.left;
	const auto adult = face_like(axis_vector(1, 0.001f, 1), box);
	auto child = face_like(axis_vector(2, 0.001f, 1), box);
	child.frontal = 40;
	auto missing = adult;
	missing.hash = {};
	for (const auto name : {"portrait.jpg", "portrait2.jpg", "portrait3.jpg", "child.jpg", "nohash.jpg"})
	{
		f.add(name, {name == "child.jpg"sv ? child : name == "nohash.jpg"sv ? missing : adult});
		f.marks[f.path(name)] = {{box, 0, cleo}};
	}
	f.add("child-like.jpg", {child});
	auto intermediate = adult;
	faces::face_vector v{};
	v[1] = 0.6f;
	v[3] = 0.8f;
	intermediate.hash = faces::quantize(v);
	f.add("intermediate.jpg", {intermediate});
	const auto bridge_only = f.add("bridge-only.jpg", {face_like(axis_vector(3, 0.001f, 1), box)});
	auto t = faces::defaults();
	t.exemplars = 2;
	const auto o = f.compare([](const core::file_path path) -> faces::picture_evidence
	{ return {{}, path.name() == "child.jpg" ? 1980 : 2010}; }, t);
	const auto* p = o->find_person("Cleo");
	assert_equal(true, p && p->confirmed.size() == 5 && p->suggested.size() == 2,
	             "the rare childhood look remains usable in a bounded profile"sv);
	assert_equal(true, p->samples.size() == 3 && std::ranges::find(p->samples, 3u) != p->samples.end(),
	             "the preview shows the childhood profile, not three similar adult portraits"sv);
	assert_equal(true,
	             p->confirmed.back().score == faces::not_compared &&
	                 std::ranges::is_sorted(p->confirmed, std::greater{}, &faces::person_member::score) &&
	                 std::ranges::is_sorted(p->suggested, std::greater{}, &faces::person_member::score),
	             "best matches first in both sections, no invented score for a missing hash"sv);
	assert_equal(true, o->person_of_face(bridge_only) == nullptr, "a suggestion cannot train a second suggestion"sv);
}

// A face whose likeness to each person's one confirmed face, on an axis of its own, is exactly the percentage given.
static faces::found_face face_alike(const rectd box, const std::array<int, 4>& percent)
{
	faces::found_face face;
	face.box = box;
	face.hash.scale = 0.01f;
	for (size_t axis = 0; axis < percent.size(); ++axis) face.hash.values[axis] = static_cast<int8_t>(percent[axis]);
	return face;
}

// People each confirmed in one picture, the first's face on axis 0, the next's on axis 1, and so on.
static std::vector<uint32_t> people_on_axes(recognition_fixture& f, const std::initializer_list<std::string_view> names)
{
	std::vector<uint32_t> ids;
	for (const auto name : names)
	{
		faces::found_face anchor;
		anchor.box = f.left;
		anchor.hash.scale = 1.0f;
		anchor.hash.values[ids.size()] = 1;
		const auto file = std::format("{}.jpg", name);
		ids.push_back(f.book.create(name));
		f.add(file, {anchor});
		f.marks[f.path(file)] = {{f.left, 0, ids.back()}};
	}
	return ids;
}

// docs/faces.md#95-naming-a-face: naming a face offers, at a press, the person it most likely is - and those
// Diffractor cannot tell from them, as brothers and sisters, beside them.
static void should_offer_whom_a_face_most_likely_is_when_naming_it()
{
	recognition_fixture f;
	people_on_axes(f, {"Amy", "Ben", "Cal", "Dee"});
	const auto siblings = f.add("siblings.jpg", {face_alike(f.left, {60, 57, 56, 40})});
	const auto clear = f.add("clear.jpg", {face_alike(f.left, {70, 60, 0, 0})});
	const auto stranger = f.add("stranger.jpg", {face_alike(f.left, {30, 20, 10, 0})});
	const auto group_a = f.add("group-a.jpg", {face_alike(f.left, {62, 40, 0, 0})});
	const auto group_b = f.add("group-b.jpg", {face_alike(f.left, {48, 55, 0, 0})});
	const auto o = f.compare();
	using names = std::vector<std::string>;
	const auto offered = [&o](const std::vector<uint32_t>& faces, const names& leave_out = {})
	{
		names result;
		for (const auto& p : faces::likely_people(*o, faces, leave_out)) result.push_back(p.name);
		return result;
	};

	assert_equal(true, o->person_of_face(siblings) == nullptr && offered({siblings}) == names{"Amy", "Ben", "Cal"},
	             "a face Diffractor cannot tell between three people offers all three, the most alike first"sv);
	const auto* const suggested = o->person_of_face(clear);
	assert_equal(true, suggested && suggested->name == "Amy" && offered({clear}) == names{"Amy"},
	             "a clear likeness offers the one person it is suggested for"sv);
	assert_equal(true, offered({clear}, {"amy"}) == names{"Ben"},
	             "named as someone else, it offers the next most like it"sv);
	assert_equal(true, offered({stranger}).empty(), "nobody it resembles less than similar"sv);
	assert_equal(true, offered({group_b}) == names{"Ben"} && offered({group_a, group_b}) == names{"Amy"},
	             "faces named together are judged by their likeness together"sv);
}

// docs/faces.md#95-naming-a-face: nobody is offered for a face who it is ruled out for, who is confirmed beside it in
// its picture, or whom it resembles no more than a face ruled out for them.
static void should_not_offer_whom_a_face_cannot_be_when_naming_it()
{
	const auto only = [](const faces::overview& o, const uint32_t face)
	{
		const auto offered = faces::likely_people(o, std::span(&face, 1), {});
		return offered.size() == 1 ? offered.front().name : std::string{};
	};
	{
		recognition_fixture f;
		const auto ids = people_on_axes(f, {"Amy", "Ben"});
		const auto face = f.add("not-amy.jpg", {face_alike(f.left, {60, 58, 0, 0})});
		f.marks[f.path("not-amy.jpg")] = {{f.left, 0, 0, false, {ids[0]}}};
		assert_equal("Ben"s, only(*f.compare(), face), "not a person it is ruled out for"sv);
	}
	{
		recognition_fixture f;
		const auto ids = people_on_axes(f, {"Amy", "Ben"});
		faces::found_face amy;
		amy.box = f.left;
		amy.hash.scale = 0.5f;
		amy.hash.values[0] = 2;
		const auto first = f.add("pair.jpg", {amy, face_alike(f.right, {65, 62, 0, 0})});
		f.marks[f.path("pair.jpg")] = {{f.left, 0, ids[0]}};
		assert_equal("Ben"s, only(*f.compare(), first + 1), "not a person confirmed beside it in its picture"sv);
	}
	{
		recognition_fixture f;
		const auto ids = people_on_axes(f, {"Amy", "Ben"});
		faces::found_face not_ben;
		not_ben.box = f.left;
		not_ben.hash.scale = 1.0f;
		not_ben.hash.values[2] = 1;
		f.add("not-ben.jpg", {not_ben});
		f.marks[f.path("not-ben.jpg")] = {{f.left, 0, 0, false, {ids[1]}}};
		const auto face = f.add("like-not-ben.jpg", {face_alike(f.left, {58, 60, 70, 0})});
		assert_equal("Amy"s, only(*f.compare(), face), "nor one it resembles no more than a face ruled out for them"sv);
	}
}

// docs/faces.md#95-naming-a-face: before anything is typed, naming lists the people the faces most likely are, then the
// names given this session, then those in the most pictures, never whom the faces are shown as; typed, the names that
// match in that order, and a new person where nobody is called what was typed. Only the most likely starts chosen, and
// a double-click answers the popup.
static void should_list_whom_to_name_a_face_most_likely_first()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const std::vector<person_word> collection = {{"Tom", {}, 210},
	                                             {"Sam", {}, 152},
	                                             {"Ella", {}, 98},
	                                             {"Grandma", {}, 64},
	                                             {"Sara", {}, 12}};
	const person_vocabulary people = [&collection](const std::string_view query, const size_t limit)
	{
		std::vector<person_word> found;
		for (const auto& w : collection)
			if (w.text.size() >= query.size() &&
			    core::icmp(std::string_view(w.text).substr(0, query.size()), query) == 0)
				found.push_back(w);
		if (found.size() > limit) found.resize(limit);
		return found;
	};
	using names = std::vector<std::string>;
	const auto dialog = std::make_shared<answered_test_frame>();
	const auto picker = make_person_picker_for_test(state, dialog, {"Sam", "Sara"}, {"Ella"}, {"Grandma"}, true,
	                                                people);
	const auto list = [&picker](const std::string_view query)
	{
		ui::auto_complete_results rows;
		picker->search(std::string(query), [&rows](const ui::auto_complete_results& r) { rows = r; });
		return rows;
	};
	const auto named = [](const ui::auto_complete_results& rows)
	{
		names result;
		for (const auto& r : rows) result.push_back(r->edit_text());
		return result;
	};

	auto rows = list({});
	assert_equal(true, named(rows) == names{"Sam", "Sara", "Ella", "Tom"} && picker->auto_select_first,
	             "the most likely first, and chosen, then the name given lately, then the most photographed"sv);
	rows = list("sa");
	const auto added = rows.empty() ? nullptr : std::dynamic_pointer_cast<text_match>(rows.back());
	assert_equal(true,
	             named(rows) == names{"Sam", "Sara", "sa"} && added &&
	                 added->label == str_format(tt.faces_new_person_fmt, "sa") && picker->auto_select_first,
	             "typed, the names that start so, the first chosen, and last a new person"sv);
	assert_equal(true, named(list("ella")) == names{"Ella"}, "a name somebody has is not a new person"sv);
	assert_equal(true, named(list("grandma")).empty(), "nor is whom the faces are shown as offered"sv);
	assert_equal(true, named(list("Zac")) == names{"Zac"} && picker->auto_select_first,
	             "a name nobody has is a new person, chosen"sv);

	const auto renaming = make_person_picker_for_test(state, dialog, {"Sam"}, {}, {}, false, people);
	ui::auto_complete_results first;
	renaming->search({}, [&first](const ui::auto_complete_results& r) { first = r; });
	assert_equal(true, !first.empty() && !renaming->auto_select_first, "a rename starts with nobody chosen"sv);

	rows = list({});
	picker->selected(rows.front(), ui::complete_strategy_t::select_type::click);
	assert_equal(false, dialog->cancelled.has_value(), "a click chooses a row"sv);
	picker->selected(rows.front(), ui::complete_strategy_t::select_type::double_click);
	assert_equal(true, dialog->cancelled == false && picker->selected() == rows.front(),
	             "a double-click answers the popup with it"sv);
}

// docs/faces.md#93-faces-on-a-picture: Ctrl+F2 names the face of a picture with just one, and says why there is none to
// name otherwise.
static void should_name_the_only_face_of_a_picture()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto folder = core::folder_path("C:\\pictures");
	const auto single = core::file_path(folder, "single.jpg");
	const auto pair = core::file_path(folder, "pair.jpg");
	const auto store = std::make_shared<faces::face_store>();
	faces::face_record record;
	record.model = faces::model_id();
	record.faces.resize(1);
	record.faces[0].box = {0.1, 0.1, 0.2, 0.2};
	store->add(single, record);
	record.faces.resize(2);
	record.faces[1].box = {0.6, 0.1, 0.2, 0.2};
	store->add(pair, record);
	store->publish();
	const auto old_store = faces::active_store();
	const auto old_overview = faces::active_overview();
	const core::scope_exit restore([&]
	{
		faces::set_active_store(old_store);
		faces::set_active_overview(old_overview);
	});
	faces::set_active_store(store);
	faces::set_active_overview(nullptr);

	assert_equal(tt.faces_name_picture_off.sv(), only_face_on_picture(state, single).refusal,
	             "nothing is named while face search is off"sv);
	index.faces().configure(true, true, true);
	const auto target = only_face_on_picture(state, single);
	assert_equal(true, target.face == 0u && target.refusal.empty(), "a picture's only face is the one named"sv);
	assert_equal(tt.faces_name_picture_several.sv(), only_face_on_picture(state, pair).refusal,
	             "of two, the one to name is pointed at"sv);
	assert_equal(tt.faces_name_picture_none.sv(),
	             only_face_on_picture(state, core::file_path(folder, "nobody.jpg")).refusal,
	             "a picture without a face has none to name"sv);
	index.faces().book_opened(core::file_path{}, faces::book_keeping::shared);
	assert_equal(tt.faces_read_only.sv(), only_face_on_picture(state, single).refusal,
	             "nor is one named while another window changes the face book"sv);
}

static void should_collapse_surnames_independently()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto view = std::make_shared<faces_view>(state, nullptr);
	auto model = std::make_shared<faces_view_model>();
	for (const auto* name : {"Jane Smith", "Barry Lane", "Ben Smith", "Vera Lane"})
		model->people.emplace_back().name = name;
	view->model_arrived_for_tests(model);
	ui::test::fake_measure_context mc;
	mc.scale_metrics(1);
	view->layout(mc, {1000, 800});
	const auto at = [&](const std::string_view title)
	{
		const auto& rows = view->rows();
		const auto r = std::ranges::find(rows, title, &faces_view::row::title);
		return r == rows.end() ? -1 : static_cast<int>(r - rows.begin());
	};
	assert_equal(true, at("Lane") < at("Smith") && at("Barry Lane") < at("Vera Lane"), "families then names");
	assert_equal(true, at("Ben Smith") + 1 == at("Jane Smith"), "one row per person");
	view->select_rows({at("Ben Smith")});
	view->toggle_surname(at("Smith"));
	assert_equal(true, view->row_bounds(at("Ben Smith")).is_empty() && !view->row_bounds(at("Barry Lane")).is_empty(),
	             "only the chosen family collapses");
	assert_equal(true,
	             view->groups_scroller()._mark_centres == std::vector{view->rows()[at("Smith")].bounds.center().y},
	             "a selected person folded away is marked at their own heading");
	view->key_down(platform::keys::RIGHT, {});
	assert_equal(false, view->row_bounds(at("Ben Smith")).is_empty(), "Right expands the focused family");
	view->key_down(platform::keys::LEFT, {});
	view->toggle_surname(at("Lane"));
	view->filter_people("bEn");
	assert_equal(true, at("Smith") >= 0 && at("Lane") < 0 && !view->row_bounds(at("Ben Smith")).is_empty(),
	             "typing reveals matching children, including folded ones");
	view->filter_people("No such person");
	assert_equal(true,
	             view->rows().size() == 2 && view->rows()[0].what == faces_view::row::kind::unknown &&
	                 view->rows()[1].what == faces_view::row::kind::hidden_faces,
	             "both empty inboxes remain available with no matching names");
	view->filter_people({});
	assert_equal(true, view->surname_collapsed("SMITH") && view->surname_collapsed("Lane"),
	             "clearing the filter restores each family's state");
	view->toggle_surname(at("Lane"));
	assert_equal(true, !view->surname_collapsed("Lane") && view->surname_collapsed("Smith"),
	             "one heading remains folded when the other opens");
	view->deactivate();
}

// docs/faces.md#91-people: a heading gathers a family of two or more, and anyone else stands alone, filed by their family
// name - found before a suffix, and first in a Chinese, Japanese or Korean name written with a space.
static void should_head_only_families_of_two_or_more()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto view = std::make_shared<faces_view>(state, nullptr);
	auto model = std::make_shared<faces_view_model>();
	for (const auto* name : {"Jane Smith", "Ben Smith", "Martin King Jr.", "Coretta King", "Ann Lee",
	                         "\u5c71\u7530 \u592a\u90ce", "\u5c71\u7530 \u82b1\u5b50", "\u738b\u5c0f\u660e", "Cher"})
		model->people.emplace_back().name = name;
	view->model_arrived_for_tests(model);
	ui::test::fake_measure_context mc;
	mc.scale_metrics(1);
	view->layout(mc, {1000, 800});
	const auto& rows = view->rows();
	const auto at = [&](const std::string_view title)
	{
		const auto r = std::ranges::find(rows, title, &faces_view::row::title);
		return r == rows.end() ? -1 : static_cast<int>(r - rows.begin());
	};
	const auto heading = [&](const std::string_view title)
	{
		return at(title) >= 0 && rows[at(title)].what == faces_view::row::kind::surname;
	};
	assert_equal(true, heading("Smith") && heading("King") && heading("\u5c71\u7530"),
	             "two who share a family name, after a suffix or before a given name, are headed");
	assert_equal(true, !heading("Lee") && at("Ann Lee") >= 0 && rows[at("Ann Lee")].surname.empty(),
	             "one person alone with their family name is not");
	assert_equal(true, at("\u738b\u5c0f\u660e") >= 0 && at("Cher") >= 0 && rows[at("Cher")].surname.empty(),
	             "nor a name of one word");
	assert_equal(true, at("Cher") < at("King") && at("King") < at("Ann Lee") && at("Ann Lee") < at("Smith"),
	             "everyone filed by family name, or their name where it has none");
	assert_equal(true, at("Coretta King") == at("King") + 1 && at("Martin King Jr.") == at("King") + 2,
	             "each family's people under its heading");
	view->deactivate();
}

static void should_review_name_and_undo_selected_faces()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto view = std::make_shared<faces_view>(state, nullptr);
	auto& book = index.faces().book();
	const auto ann = book.create("Ann Lane");
	const rectd box{0.1, 0.1, 0.3, 0.3};
	const auto store = std::make_shared<faces::face_store>();
	for (int i = 0; i != 6; ++i)
	{
		faces::face_record record;
		record.model = faces::model_id();
		record.faces = {face_like(axis_vector(1, 0.04f, i), box)};
		store->add(core::file_path(core::folder_path("C:\\pictures"), std::format("portrait-{}.jpg", i)), record);
	}
	store->publish();
	// A published result, independent of matcher thresholds: three confirmed portraits, two
	// suggestions, and a sixth unknown with no offer at all.
	auto model = std::make_shared<faces_view_model>();
	model->store = store;
	auto o = std::make_shared<faces::overview>();
	model->overview = o;
	o->store = store;
	o->book_generation = book.generation();
	o->people.emplace_back();
	auto& p = o->people.front();
	p.id = ann;
	p.name = "Ann Lane";
	p.pictures = 3;
	p.samples = {0, 1, 2};
	for (uint32_t i = 0; i < 5; ++i)
	{
		const auto confirmed = i < 3;
		auto& list = confirmed ? p.confirmed : p.suggested;
		o->members[i] = {0, static_cast<uint32_t>(list.size()), confirmed};
		list.push_back(
			{i, 1.0f - i * 0.05f, confirmed ? faces::member_kind::confirmed : faces::member_kind::suggested});
	}
	o->unknown = {5};
	view->model_arrived_for_tests(model);
	ui::test::fake_measure_context mc;
	mc.scale_metrics(1.0);
	view->layout(mc, {1200, 900});
	const auto row = [&](const std::string_view title)
	{
		const auto& rows = view->rows();
		return static_cast<int>(std::ranges::find(rows, title, &faces_view::row::title) - rows.begin());
	};
	const auto cell = [&](uint32_t face)
	{
		const auto& cells = view->cells();
		return static_cast<int>(std::ranges::find_if(cells, [face](const auto& c) { return c.shown.face == face; }) -
		                        cells.begin());
	};

	assert_equal(true, view->faces_title() == tt.unknown.sv() && view->cells().size() == 1, "the default inbox");
	view->select_cells({cell(5)});
	assert_equal(true,
	             view->can_name_faces() && view->can_search_face() && view->can_hide() && !view->can_rename() &&
	                 !view->can_confirm() && !view->can_reject(),
	             "unknown faces can be named or compared, not silently confirmed");
	view->select_rows({row("Ann Lane")});
	view->layout(mc, {1200, 900});
	assert_equal(3_z, view->rows()[row("Ann Lane")].previews.size(), "three distinct representative portraits");
	assert_equal(true,
	             view->headings().size() == 2 && view->headings()[0].text == tt.faces_confirmed.sv() &&
	                 view->headings()[1].text == tt.faces_suggested.sv() && view->headings()[1].before == 3 &&
	                 view->cells()[3].score > view->cells()[4].score,
	             "confirmed, then offers best match first");
	view->click_cell(0, {});
	// The corner icons review the hovered face, not the current selection.
	for (const auto corner : {faces_view::suggestion_action::reject, faces_view::suggestion_action::confirm})
	{
		const auto confirm = corner == faces_view::suggestion_action::confirm;
		const auto i = cell(3);
		const auto at = view->suggestion_button_bounds(i, corner).center();
		hit_test_context ctx(at, recti(0, 0, 1200, 900));
		auto action = view->controller_from_location({}, at, ctx);
		assert_equal(true, action != nullptr, "suggestion corner has a hit target");
		if (!action) return;
		action->on_mouse_left_button_down(at, {});
		action->on_mouse_left_button_up(view->cell_bounds(i).center(), {});
		assert_equal(true, book.marks(store->run_of_face(3).path).empty(), "releasing outside cancels the edit");
		action->on_mouse_left_button_down(at, {});
		action->on_mouse_left_button_up(at, {});
		action.reset();
		const auto marks = book.marks(store->run_of_face(3).path);
		assert_equal(true,
		             marks.size() == 1 &&
		                 (confirm ? marks.front().person == ann : marks.front().rejected == std::vector{ann}),
		             "the corner applies the indicated decision");
		assert_equal(true, view->selected_cells() == std::vector{0} && view->can_undo_placement(),
		             "unrelated selection survives hover review");
		assert_equal(true, view->status().find('\n') == std::string_view::npos, "only the receipt is shown");
		assert_equal(true,
		             view->undo_detail().find(confirm ? "Confirm Ann Lane" : "Not Ann Lane") !=
		                     std::string_view::npos &&
		                 view->undo_detail().find("portrait-3.jpg") != std::string_view::npos,
		             "Undo identifies the exact edit and photo");
		view->undo_placement();
		assert_equal(true, book.marks(store->run_of_face(3).path).empty(), "the hover edit is reversible");
		view->layout(mc, {1200, 900});
	}
	{
		// docs/faces.md#95-naming-a-face: the third corner names the face as someone else, asking for the name.
		const auto i = cell(3);
		const auto name = view->suggestion_button_bounds(i, faces_view::suggestion_action::name);
		assert_equal(true,
		             !name.intersects(view->suggestion_button_bounds(i, faces_view::suggestion_action::confirm)) &&
		                 !name.intersects(view->suggestion_button_bounds(i, faces_view::suggestion_action::reject)) &&
		                 name == name.intersection(view->cell_bounds(i)),
		             "naming as someone else has a corner of the face's own");
		hit_test_context ctx(name.center(), recti(0, 0, 1200, 900));
		auto action = view->controller_from_location({}, name.center(), ctx);
		assert_equal(true, action != nullptr, "and a hit target");
		if (!action) return;
		view_hover_element bubble;
		bubble.clear();
		action->popup_from_location(bubble);
		std::vector<std::string> lines;
		bubble.elements->visit_children([&lines](const view_element_ptr& e)
		{
			if (const auto* t = dynamic_cast<const ui::text_element_base*>(e.get())) lines.emplace_back(t->text());
		});
		assert_equal(true,
		             lines.size() == 2 && lines[0] == tt.faces_name_someone_else.sv() &&
		                 lines[1] == tt.faces_name_someone_else_hint.sv(),
		             "its bubble says what it does");
		action->on_mouse_left_button_down(name.center(), {});
		action->on_mouse_left_button_up(name.center(), {});
		action.reset();
		assert_equal(true, book.marks(store->run_of_face(3).path).empty() && view->selected_cells() == std::vector{0},
		             "with no window to ask the name in, it changes nothing");
	}
	view->click_cell(2, {.control = true});
	view->key_down(platform::keys::RIGHT, {.control = true, .shift = true});
	assert_equal(true, view->selected_cells() == std::vector{0, 2, 3}, "Ctrl+Shift adds the range from the anchor");
	view->model_arrived_for_tests(model);
	view->layout(mc, {1200, 900});
	assert_equal(true, view->selected_cells() == std::vector{0, 2, 3} && view->focus_cell() == 3,
	             "model replacement preserves the exact face selection and cursor");
	const auto a = view->cell_bounds(0).center();
	const auto b = view->cell_bounds(1).center();
	view->begin_marquee(a, false);
	view->end_marquee({b.x, b.y + 1});
	assert_equal(true, view->selected_cells() == std::vector{0, 1},
	             "a rubber band selects just the portraits it touches");
	view->context_menu(view->cell_bounds(3).center());
	assert_equal(true, view->selected_cells() == std::vector{3},
	             "a right-click scopes review to the portrait under it");
	view->select_cells({cell(3)});
	assert_equal(true, view->confirm_text() == "Confirm Ann Lane" && view->remove_text() == "Not Ann Lane",
	             "both review decisions name the actual person");
	view->key_down(platform::keys::DEL, {});
	const auto rejected = book.marks(store->run_of_face(3).path);
	assert_equal(true,
	             rejected.size() == 1 && rejected.front().rejected == std::vector{ann} && !rejected.front().hidden,
	             "Delete rejects just this person, without hiding the face");
	assert_equal(true, view->cells().size() == 4 && view->placement_destination() == tt.unknown.sv(),
	             "rejection removes the offer immediately and links to Unknown");
	view->undo_placement();
	view->click_heading(1, {});
	assert_equal(2_z, view->selected_cells().size(), "clicking Suggested selects only that review queue");
	view->confirm();
	assert_equal(true, view->headings().size() == 1 && view->cells().size() == 5 && view->can_undo_placement(),
	             "one atomic confirmation for both selected suggestions");
	view->undo_placement();
	view->select_rows({row(tt.unknown.sv())});
	view->select_cells({cell(5)});
	view->hide(true);
	view->follow_placement();
	assert_equal(true, view->faces_title() == tt.faces_hidden_section.sv() && view->cells().size() == 1,
	             "Hidden is a singleton destination for individual faces");
	view->select_cells({0});
	assert_equal(true, view->can_name_faces() && view->can_show_again() && !view->can_hide(),
	             "hidden can be named or shown");
	view->move_cells_to_name(view->selected_cells(), "Jean Lake");
	const auto marks = book.marks(store->run_of_face(5).path);
	assert_equal(true,
	             marks.size() == 1 && marks[0].person == book.people()->named("Jean Lake")->id && !marks[0].hidden,
	             "naming a hidden face unhides it");
	assert_equal(true, book.marks(store->run_of_face(3).path).empty() && book.marks(store->run_of_face(4).path).empty(),
	             "no neighbor has been assigned");
	view->undo_placement();
	assert_equal(true,
	             !book.people()->named("Jean Lake") && view->cells().size() == 1 &&
	                 book.marks(store->run_of_face(5).path).front().hidden,
	             "undo recovers hidden state and roster");
	view->select_cells({0});
	view->hide(false);
	view->select_rows({row(tt.unknown.sv())});
	assert_equal(1_z, view->cells().size(), "Show returns the unmatched face to Unknown, not its suggested neighbors");
	view->move_cells_to_name({cell(5)}, "Ann Lane");
	book.rename(ann, "Ann Moon");
	view->filter_people("does not match");
	assert_equal(true, !view->can_undo_placement() && view->placement_destination() == "Ann Lane",
	             "subsequent edits expire Undo, not the stable destination");
	view->follow_placement();
	assert_equal(true, view->filter_text().empty() && view->faces_title() == "Ann Moon",
	             "follow reveals the renamed person rather than matching the old text");
	view->deactivate();
}

static void should_keep_faces_scrolled_at_suggested_after_review()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto host = std::make_shared<view_controls_host>(state);
	const auto view = std::make_shared<faces_view>(state, host);
	auto& book = index.faces().book();
	const auto ann = book.create("Ann");
	const auto saved = setting.faces.tile_size;
	const core::scope_exit restore([saved] { setting.faces.tile_size = saved; });
	setting.faces.tile_size = 104;

	const auto store = std::make_shared<faces::face_store>();
	for (uint32_t i = 0; i < 80; ++i)
	{
		faces::face_record record;
		record.model = faces::model_id();
		record.faces = {face_like(axis_vector(1, 0.02f, static_cast<int>(i)), {0.1, 0.2, 0.2, 0.3})};
		store->add(recognition_fixture::path(std::format("portrait-{}.jpg", i)), record);
	}
	store->publish();
	const auto review = [&](const uint32_t confirmed, const uint32_t total)
	{
		auto model = std::make_shared<faces_view_model>();
		model->store = store;
		auto overview = std::make_shared<faces::overview>();
		model->overview = overview;
		overview->store = store;
		overview->book_generation = book.generation();
		auto& person = overview->people.emplace_back();
		person.id = ann;
		person.name = "Ann";
		for (uint32_t i = 0; i < total; ++i)
		{
			const auto held = i < confirmed;
			auto& list = held ? person.confirmed : person.suggested;
			overview->members[i] = {0, static_cast<uint32_t>(list.size()), held};
			list.push_back({i, 1.0f - static_cast<float>(i) / 100,
			                held ? faces::member_kind::confirmed : faces::member_kind::suggested});
		}
		return model;
	};

	ui::test::fake_measure_context mc;
	mc.scale_metrics(1.0);
	constexpr sizei extent{720, 440};
	view->model_arrived_for_tests(review(12, 64));
	view->select_rows({2});
	view->layout(mc, extent);
	assert_equal(2_z, view->headings().size(), "the person has confirmed and suggested faces");
	const auto heading_y = [&]
	{
		return view->headings().back().bounds.top - view->faces_scroller().scroll_offset().y;
	};
	const auto heading_top = view->headings().back().bounds.top;
	assert_equal(true, view->mouse_wheel({500, 220}, {.delta = 20 - heading_top, .keys = {}}),
	             "Suggested can be brought near the top of the pane");
	const auto y = heading_y();
	assert_equal(20, y, "the heading starts in view");
	view->select_cells({12, 13, 14, 15});
	view->confirm();
	view->layout(mc, extent);
	assert_equal(16_z, view->headings().back().before, "confirmation adds a row above Suggested");
	assert_equal(true, view->headings().back().bounds.top > heading_top,
	             "the header's content position really changed");
	assert_equal(y, heading_y(), "the header's screen position survives even though the cursor moved above it");

	view->model_arrived_for_tests(review(16, 80));
	view->layout(mc, extent);
	assert_equal(80_z, view->cells().size(), "the refreshed overview adds new suggestions");
	assert_equal(y, heading_y(), "the new suggestions don't move the heading either");

	view->select_rows({0});
	view->layout(mc, extent);
	assert_equal(0, view->faces_scroller().scroll_offset().y, "switching to another page starts at its top");
	view->deactivate();
}

// docs/faces.md#91-people: naming faces in Unknown closes the list up below the top of the page rather than moving what
// is above them; the overview that follows, its runs re-formed as the name took lookalikes as suggestions, keeps the
// runs in the order they were shown, each under its heading; and choosing Unknown again sorts them afresh.
static void should_keep_unknown_in_place_as_its_faces_are_named()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto host = std::make_shared<view_controls_host>(state);
	const auto view = std::make_shared<faces_view>(state, host);
	auto& book = index.faces().book();
	const auto saved_tile = setting.faces.tile_size;
	const auto saved_width = setting.faces.groups_width;
	const core::scope_exit restore([saved_tile, saved_width]
	{
		setting.faces.tile_size = saved_tile;
		setting.faces.groups_width = saved_width;
	});
	setting.faces.tile_size = 104;
	setting.faces.groups_width = 200;

	const auto store = std::make_shared<faces::face_store>();
	for (uint32_t i = 0; i < 40; ++i)
	{
		faces::face_record record;
		record.model = faces::model_id();
		record.faces = {face_like(axis_vector(static_cast<int>(i), 0.02f, static_cast<int>(i)), {0.1, 0.2, 0.2, 0.3})};
		store->add(recognition_fixture::path(std::format("face-{}.jpg", i)), record);
	}
	store->publish();
	const auto faces_from = [](const uint32_t from, const uint32_t to)
	{
		std::vector<uint32_t> result;
		for (auto f = from; f < to; ++f) result.push_back(f);
		return result;
	};
	// Unknown as an overview orders it: the runs, largest first, then the faces alone.
	const auto unknown = [&](const std::vector<std::vector<uint32_t>>& runs, const std::vector<uint32_t>& alone)
	{
		auto overview = std::make_shared<faces::overview>();
		overview->store = store;
		overview->book_generation = book.generation();
		for (const auto& run : runs)
		{
			overview->unknown_runs.push_back(static_cast<uint32_t>(run.size()));
			overview->unknown_by_similarity.insert(overview->unknown_by_similarity.end(), run.begin(), run.end());
		}
		overview->unknown_by_similarity.insert(overview->unknown_by_similarity.end(), alone.begin(), alone.end());
		overview->unclear_from = overview->unknown_by_similarity.size();
		overview->unknown = overview->unknown_by_similarity;
		std::ranges::sort(overview->unknown);
		auto model = std::make_shared<faces_view_model>();
		model->store = store;
		model->overview = overview;
		return model;
	};
	const auto heading = [&](const std::string_view key) -> const faces_view::heading*
	{
		const auto at = std::ranges::find(view->headings(), key, &faces_view::heading::key);
		return at == view->headings().end() ? nullptr : &*at;
	};
	const auto y_of = [&](const std::string_view key)
	{
		const auto* h = heading(key);
		return h ? h->bounds.top - view->faces_scroller().scroll_offset().y : std::numeric_limits<int>::min();
	};
	const auto keys = [&]
	{
		std::vector<std::string> result;
		for (const auto& h : view->headings()) result.push_back(h.key);
		return result;
	};

	ui::test::fake_measure_context mc;
	mc.scale_metrics(1.0);
	constexpr sizei extent{720, 640};
	view->model_arrived_for_tests(
		unknown({faces_from(0, 10), faces_from(10, 19), faces_from(19, 24), faces_from(24, 27)}, faces_from(27, 40)));
	view->layout(mc, extent);
	assert_equal(true, keys() == std::vector<std::string>{"run:0", "run:10", "run:19", "run:24", "other"},
	             "Unknown lists its runs largest first, then the faces alone");
	assert_equal(true, view->mouse_wheel({500, 300}, {.delta = -heading("run:10")->bounds.top, .keys = {}}),
	             "the second run can be brought to the top of the page");
	assert_equal(0, y_of("run:10"), "its heading starts the page");
	const auto next_y = y_of("run:19");
	const auto& cells = view->cells();
	const auto first = heading("run:10")->before;
	std::vector<int> first_row;
	for (auto i = first; i < cells.size() && cells[i].bounds.top == cells[first].bounds.top; ++i)
		first_row.push_back(static_cast<int>(i));
	assert_equal(true, first_row.size() > 1 && first_row.size() < 9 && next_y > 0 && next_y < extent.cy,
	             "the run fills more than a row, and the next run's heading is in view");

	view->move_cells_to_name(first_row, "Bea");
	view->layout(mc, extent);
	assert_equal(0, y_of("run:10"), "naming the faces at the top of the page leaves the run they were in where it was");
	assert_equal(true, y_of("run:19") < next_y, "and the runs below close up into the room they left");

	// The overview that follows: the named faces are Bea's, and her lookalikes are suggested for her - the next of the
	// run named from, and two of the run after it - while the last run gains faces that were alone. Sorted afresh, that
	// run would come second; Unknown keeps the order it showed, and each run its heading.
	const auto named = first_row.size();
	view->model_arrived_for_tests(unknown({faces_from(0, 10), faces_from(24, 33),
	                                       faces_from(10 + static_cast<uint32_t>(named) + 1, 19), faces_from(21, 24)},
	                                      faces_from(33, 40)));
	view->layout(mc, extent);
	assert_equal(true, keys() == std::vector<std::string>{"run:0", "run:10", "run:19", "run:24", "other"},
	             "the runs keep the order they were shown in, and their headings, though their first faces left them");
	assert_equal(0, y_of("run:10"), "and the run at the top of the page stays there");

	view->select_rows({0});
	view->layout(mc, extent);
	assert_equal(true, keys().size() == 5 && keys()[1] == "run:24",
	             "choosing Unknown again sorts it afresh, the largest runs first");
	view->deactivate();
}

static void should_scroll_resize_and_mark_the_people_list()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto host = std::make_shared<view_controls_host>(state);
	const auto view = std::make_shared<faces_view>(state, host);
	const auto saved = setting.faces.groups_width;
	const core::scope_exit restore([&] { setting.faces.groups_width = saved; });
	setting.faces.groups_width = 200;

	auto model = std::make_shared<faces_view_model>();
	for (int i = 0; i < 24; ++i) model->people.emplace_back().name = std::format("Person {}", i);
	view->model_arrived_for_tests(model);
	ui::test::fake_measure_context mc;
	mc.scale_metrics(1.0);
	constexpr sizei extent{800, 401};
	const auto layout = [&]
	{
		view->layout(mc, extent);
	};
	const auto hit = [&](const pointi at)
	{
		hit_test_context ctx(at, recti(extent));
		return view->controller_from_location(host, at, ctx);
	};

	layout();
	const auto& list = view->groups_scroller();
	const auto bar = list.scroll_bounds();
	const pointi upper{bar.left + 1, bar.top - 1};
	const auto resize = hit(upper);
	assert_equal(true, resize && resize->cursor() == platform::cursor::left_right, "upper half resizes");
	assert_equal(true,
	             resize->bounds() == recti(bar.left, 0, bar.right, extent.cy / 2) &&
	                 bar == recti(list.client_bounds().right, extent.cy / 2, 200, extent.cy) &&
	                 bar.width() == mc.scroll_width && view->faces_scroller().client_bounds().left == bar.right,
	             "one sidebar-width column between list and faces");

	view->select_rows({0, 11, 21});
	layout();
	const auto& rows = view->rows();
	const std::vector<int> centres{rows[0].bounds.center().y, rows[11].bounds.center().y, rows[21].bounds.center().y};
	assert_equal(true, list._mark_centres == centres && view->row_bounds(21).is_empty(), "offscreen people marked");
	ui::test::fake_draw_context dc;
	dc.scale_metrics(1.0);
	view->render(dc, nullptr);
	const auto blue = core::color(ui::style::colors.view_selected_background, dc.colors.alpha);
	const auto marks = std::ranges::count_if(dc.fills, [&](const auto& fill)
	{
		return fill.rect.left >= bar.left && fill.rect.right <= bar.right && fill.rect.top >= bar.top &&
		       fill.rect.height() == 2 && fill.color == blue;
	});
	assert_equal(3, static_cast<int>(marks), "one blue mark on the track for each separated selection");

	const auto grab = list.thumb_bounds().center();
	const auto scroll = hit(grab);
	assert_equal(true, scroll && scroll->cursor() == platform::cursor::up_down, "lower half scrolls");
	scroll->on_mouse_left_button_down(grab, {});
	scroll->on_mouse_move({grab.x + 40, grab.y + 60});
	scroll->on_mouse_left_button_up({grab.x + 40, grab.y + 60}, {});
	const auto y = list.scroll_offset().y;
	assert_equal(true,
	             y > 0 && view->row_bounds(0).is_empty() && list._mark_centres == centres &&
	                 view->faces_scroller().scroll_offset().y == 0 && setting.faces.groups_width == 200,
	             "dragging the thumb scrolls only people and does not move their marks");
	assert_equal(true,
	             view->can_scroll(upper) && view->mouse_wheel(upper, {.delta = -20, .keys = {}}) &&
	                 list.scroll_offset().y == y + 20,
	             "the wheel over the divider belongs to People");

	const pointi wider{upper.x + 50, upper.y + 80};
	resize->on_mouse_left_button_down(upper, {});
	resize->on_mouse_move(wider);
	layout();
	assert_equal(250, list.scroll_bounds().right, "horizontal drag keeps its grab point across both halves");
	assert_equal(y + 20, list.scroll_offset().y, "resizing does not scroll");
	assert_equal(true, resize->escape(), "Escape cancels a resize");
	resize->on_mouse_left_button_up(wider, {});
	layout();
	assert_equal(200, list.scroll_bounds().right, "cancel and release restore the width");
	resize->on_mouse_left_button_down(upper, {});
	resize->on_mouse_left_button_up(wider, {});
	layout();
	assert_equal(250, setting.faces.groups_width, "release remembers the new width");

	view->filter_people("Person 21");
	layout();
	assert_equal(true, !list.can_scroll() && list._mark_centres == std::vector{view->rows().front().bounds.center().y},
	             "filtering removes stale marks and moves the surviving one with its row");
	const auto full = hit(list.scroll_bounds().center());
	assert_equal(true,
	             full && full->cursor() == platform::cursor::left_right &&
	                 full->bounds() == recti(list.client_bounds().right, 0, 250, extent.cy),
	             "when the rows fit, even the lower half resizes");
	view->select_rows({});
	assert_equal(true, list._mark_centres.empty(), "clearing the selection clears the marks");
	view->deactivate();
}

// docs/faces.md#91-people: a name too long for the People list ends in an ellipsis, as a cell too narrow for its text
// does, rather than being cut with nothing to say so; one that fits is drawn whole.
static void should_end_a_name_too_long_for_the_people_list_in_an_ellipsis()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto host = std::make_shared<view_controls_host>(state);
	const auto view = std::make_shared<faces_view>(state, host);
	const auto saved = setting.faces.groups_width;
	const core::scope_exit restore([&] { setting.faces.groups_width = saved; });
	setting.faces.groups_width = 200;

	constexpr auto long_name = "Maximiliana Konstantinopoulou-Papadopoulos"sv;
	auto model = std::make_shared<faces_view_model>();
	model->people.emplace_back().name = long_name;
	model->people.emplace_back().name = "Ann";
	view->model_arrived_for_tests(model);
	ui::test::fake_measure_context mc;
	mc.scale_metrics(1.0);
	view->layout(mc, {800, 400});
	ui::test::fake_draw_context dc;
	dc.scale_metrics(1.0);
	view->render(dc, nullptr);

	const auto drawn = [&dc](const std::string_view start)
	{
		return std::ranges::find_if(dc.texts, [start](const auto& t) { return t.text.starts_with(start); });
	};
	const auto cut = drawn("Maximiliana");
	assert_equal(true,
	             cut != dc.texts.end() && cut->text.ends_with("\xE2\x80\xA6"sv) &&
	                 cut->text.size() < long_name.size() && dc.metrics.measure(cut->text).cx <= cut->clip.width(),
	             "the long name ends in an ellipsis within its line"sv);
	const auto whole = drawn("Ann");
	assert_equal(true, whole != dc.texts.end() && whole->text == "Ann", "a short name is drawn whole"sv);
	view->deactivate();
}

// docs/faces.md#92-faces: pointing at a face shows its whole picture, and when and where the picture was taken - which
// tell who a face is as often as the face does.
static void should_say_when_and_where_a_faces_picture_was_taken()
{
	const auto folder = test_temps.next_folder("faces-bubble");
	const auto path = test_temps.next_path_in(folder, ".jpg");
	platform::copy_file(test_files_folder().combine_file("Test.jpg"), path, false, false);

	// Ann, named in a picture taken in Prague on 14 September 2012.
	metadata_edits edits;
	edits.faces = face_name_edits{{{face_name_edit::kind::name, rectd{0.1, 0.1, 0.2, 0.2}, "Ann", {}}},
	                              {},
	                              core::orientation::top_left};
	files ff;
	assert_equal(true, ff.update(path, edits, {}, {}, false, {}).success(), "the name is written"sv);

	null_state_strategy ss;
	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	core::index_roots roots;
	roots.folders.emplace(folder);
	index.index_roots(roots);
	index.index_folders(test_token);
	load_item(index, path, false);

	view_state state(ss, as, index, make_test_player());
	const auto view = std::make_shared<faces_view>(state, nullptr);
	view->model_arrived_for_tests(make_faces_view_model(index, nullptr, nullptr, [] { return false; }));

	const auto& rows = view->rows();
	const auto ann = std::ranges::find_if(rows, [](const auto& r) { return r.named() && r.title == "Ann"; });
	assert_equal(true, ann != rows.end(), "a row for Ann"sv);
	view->select_rows({static_cast<int>(ann - rows.begin())});
	assert_equal(1, static_cast<int>(view->cells().size()), "her region of the picture"sv);

	view_hover_element bubble;
	bubble.clear();
	ui::test::fake_measure_context mc;
	mc.scale_metrics(1.0);
	view->layout(mc, {1800, 720});
	view->bubble_cell(0, bubble);
	std::vector<std::string> lines;
	bubble.elements->visit_children([&lines](const view_element_ptr& e)
	{
		if (const auto* t = dynamic_cast<const ui::text_element_base*>(e.get())) lines.emplace_back(t->text());
	});
	const auto says = [&lines](const std::string_view text)
	{
		return std::ranges::find(lines, text) != lines.end();
	};

	assert_equal(true, says(path.name()), "the picture's name"sv);
	assert_equal(true, !lines.empty() && lines.front() == "Ann", "the person's name heads the preview"sv);
	assert_equal(true,
	             says(std::format("{} \u00b7 Prague, Hlavní Mesto Praha, Czech Republic",
	                              platform::format_date(core::date_t(2012, 9, 14, 19, 21, 14)))),
	             "date and place share a caption"sv);

	view_element_ptr picture;
	bubble.elements->visit_children([&](const view_element_ptr& e)
	{
		if (dynamic_cast<surface_element*>(e.get())) picture = e;
	});
	assert_equal(true, picture != nullptr, "hovering loads the whole photo via the crop worker"sv);
	if (picture)
	{
		const auto size = picture->measure(mc, 600);
		assert_equal(true, size.cx < 600 && size.cy == 240, "the image is limited by height, not width"sv);
		assert_equal(size.cx, bubble.preferred_size, "the preview has no extra width beside the photo"sv);
		bubble.elements->layout(mc, {0, 0, size.cx, 180});
		const auto photo = picture->bounds;
		assert_equal(true,
		             !photo.is_empty() && photo.height() < 180 && photo.width() < size.cx &&
		                 std::abs(photo.width() * size.cy - photo.height() * size.cx) <= size.cx,
		             "the image shrinks proportionally to make room for captions on a short screen"sv);
		bubble.elements->visit_children([&](const view_element_ptr& e)
		{ assert_equal(true, e->bounds.top >= 0 && e->bounds.bottom <= 180, "each caption and the photo fit"); });
	}
}

static void should_outline_the_hovered_face_on_an_upright_preview()
{
	class preview_context final : public ui::test::recording_draw_context
	{
	public:
		recti outline;
		core::color color;
		void draw_border(const recti, const recti outer, const core::color c, const core::color) override
		{
			outline = outer;
			color = c;
		}
	};
	const auto stored = std::make_shared<core::surface>();
	stored->alloc(300, 200, core::texture_format::RGB);
	stored->orientation(core::orientation::right_top);
	const auto saved = setting.show_rotated;
	const core::scope_exit restore([saved] { setting.show_rotated = saved; });
	setting.show_rotated = false;

	preview_context dc;
	dc.scale_metrics(1.0);
	dc.colors.alpha = 1;
	for (const auto& area : {rectd{0.1, 0.2, 0.25, 0.3}, rectd{0.8, 0.7, 0.25, 0.4}, rectd{}})
	{
		surface_element preview(stored, sizei{600, 600}, area);
		const auto size = preview.measure(dc, 600);
		assert_equal(true, size == sizei(400, 600), "the rotated preview fits the whole photo, not a square crop"sv);
		preview.layout(dc, {0, 0, size.cx, size.cy});
		dc.outline = {};
		preview.render(dc, {10, 20});
		const recti expected = area.Width == 0 ? recti{}
		                       : area.X < 0.5  ? recti(50, 140, 150, 320)
		                                       : recti(330, 440, 410, 620);
		assert_equal(true, dc.outline == expected, "the upright face box follows the hover, clipped to the picture"sv);
		if (!expected.is_empty())
			assert_equal(true, dc.color.b > dc.color.g && dc.color.g > dc.color.r, "the outline is blue"sv);
	}
}

// docs/faces.md#9-the-faces-view: the groups are made again after every change, and while they are the Faces view
// says so.
static void should_say_while_the_face_suggestions_are_made_again()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto view = std::make_shared<faces_view>(state, nullptr);
	index.faces().configure(true, true, false);

	const auto says_updating = [&view]
	{
		view->faces_changed();
		return view->status().find(tt.faces_updating.sv()) != std::string_view::npos;
	};

	assert_equal(false, says_updating(), "nothing being made"sv);
	index.faces().queue_overview();
	assert_equal(true, index.faces().is_grouping() && says_updating(), "a pass waiting to run"sv);
	while (as.run_next(async_queue::faces_overview))
	{
	}
	assert_equal(false, index.faces().is_grouping() || says_updating(), "and done"sv);
	as.drain_ui();
}

static void should_preview_face_search_targets_and_open_the_picture_in_its_folder()
{
	const auto folder = test_temps.next_folder("faces-search-target");
	const auto path = test_temps.next_path_in(folder, ".jpg");
	platform::copy_file(test_files_folder().combine_file("Test.jpg"), path, false, false);

	null_state_strategy ss;
	null_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	core::index_roots roots;
	roots.folders.emplace(folder);
	index.index_roots(roots);
	index.index_folders(test_token);
	load_item(index, path, false);

	const rectd box{0.1, 0.1, 0.2, 0.2};
	auto store = std::make_shared<faces::face_store>();
	faces::face_record record;
	record.model = faces::model_id();
	record.faces = {face_like(axis_vector(2, 0.02f, 1), {0.5, 0.1, 0.2, 0.2}),
	                face_like(axis_vector(1, 0.02f, 2), box)};
	store->add(path, record);
	store->publish();
	auto& book = index.faces().book();
	const auto ann = book.create("Ann Smith");
	const std::array confirmed{faces::face_ref{path, box, 1}};
	const faces::placement_batch assign_ann{ann, {}, {confirmed.front()}};
	book.place(std::span(&assign_ann, 1), faces::face_action::assign, book.generation());

	view_state state(ss, as, index, make_test_player());
	const auto view = std::make_shared<faces_view>(state, nullptr);
	auto model = std::make_shared<faces_view_model>();
	model->store = store;
	model->overview = faces::make_overview(store, no_regions, book, [] { return false; });
	view->model_arrived_for_tests(model);
	view->select_rows({static_cast<int>(std::ranges::find(view->rows(), "Ann Smith"sv, &faces_view::row::title) -
	                                    view->rows().begin())});
	ui::test::fake_measure_context mc;
	mc.scale_metrics(1.0);
	view->layout(mc, {1000, 600});
	assert_equal(1_z, view->cells().size(), "Ann's face is shown"sv);

	const auto folder_search = df::search_t().add_selector(folder);
	assert_equal(true, state.open(nullptr, folder_search, {}), "start in the face's folder"sv);
	view->open();
	assert_equal(true, view->can_search_person() && !view->can_search_face(), "the named group is targeted"sv);
	const auto check_preview = [&view](const commands id, const std::string_view text, const bool show_crop)
	{
		view_hover_element bubble;
		bubble.clear();
		view->search_tooltip(bubble, id);
		auto has_text = false;
		auto has_crop = false;
		bubble.elements->visit_children([&](const view_element_ptr& element)
		{
			if (const auto* t = dynamic_cast<const ui::text_element_base*>(element.get()))
				has_text |= t->text().find(text) != std::string::npos;
			has_crop |= dynamic_cast<const surface_element*>(element.get()) != nullptr;
		});
		assert_equal(true, has_text, "the bubble identifies the search target"sv);
		assert_equal(show_crop, has_crop, "face searches show the target crop"sv);
	};
	check_preview(commands::face_person_search, "Ann Smith", false);
	view->search_person();
	assert_equal(true,
	             state.view_mode() == view_type::items &&
	                 state.search() == df::search_t().with(prop::person, "Ann Smith"),
	             "Person search goes to an exact name search in Items"sv);
	state.browse_back(nullptr);
	assert_equal(true, state.view_mode() == view_type::faces && view->can_search_person(), "Back keeps the group"sv);

	const auto menu = view->context_menu(view->cell_bounds(0).center());
	assert_equal(true, std::ranges::find(menu.list, commands::face_open_in_folder) != menu.list.end(),
	             "the face's context menu offers Open in folder"sv);
	assert_equal(true, view->can_open_in_folder() && view->can_search_face() && !view->can_search_person(),
	             "the face, not the person's name, is targeted"sv);
	check_preview(commands::face_search, path.name(), true);
	view->search_face();
	assert_equal(true, state.view_mode() == view_type::items && state.search() == df::search_for_face(path.name(), 2),
	             "Face search opens Items directly and uses the selected face's position"sv);
	state.browse_back(nullptr);
	assert_equal(true, state.view_mode() == view_type::faces && view->is_cell_selected(0), "Back keeps the face"sv);

	state.browse_back(nullptr);
	assert_equal(true, state.view_mode() == view_type::items && state.search() == folder_search,
	             "the original folder is still in history"sv);
	view->open();
	view->open_in_folder();
	assert_equal(true, state.view_mode() == view_type::items && state.search() == folder_search,
	             "the face's folder is opened inside Items"sv);
	const auto& selected = state.selected_items().items();
	assert_equal(true, selected.size() == 1 && selected.front()->path() == path, "the picture is selected"sv);
	state.browse_back(nullptr);
	assert_equal(true, state.view_mode() == view_type::faces && view->is_cell_selected(0),
	             "Back returns to Faces even when opening the same folder"sv);
}

static void should_open_a_newly_named_picture_face_on_its_person_page()
{
	null_state_strategy ss;
	deferred_async_strategy as;
	location_cache locations;
	index_state index(as, locations);
	view_state state(ss, as, index, make_test_player());
	const auto view = std::make_shared<faces_view>(state, nullptr);
	auto& book = index.faces().book();
	const core::file_path path(core::folder_path("C:\\pictures"), "portrait.jpg");
	const rectd box{0.1, 0.1, 0.2, 0.2};
	const auto store = std::make_shared<faces::face_store>();
	faces::face_record record;
	record.model = faces::model_id();
	record.faces = {face_like(axis_vector(1, 0.04f, 1), box)};
	store->add(path, record);
	store->publish();
	const auto model_now = [&]
	{
		auto model = std::make_shared<faces_view_model>();
		model->store = store;
		model->overview = faces::make_overview(store, no_regions, book, [] { return false; });
		return model;
	};

	const auto previous = model_now();
	view->model_arrived_for_tests(previous);
	view->deactivate();
	const std::array ref{faces::face_ref{path, box, 0}};
	const faces::placement_batch assign{0, "Avery", {ref.front()}};
	book.place(std::span(&assign, 1), faces::face_action::assign, book.generation());
	view->open_at_face(0);
	view->model_arrived_for_tests(previous); // An in-flight overview still puts this face in Unknown.
	view->model_arrived_for_tests(model_now());

	const auto rows = view->selected_rows();
	const auto cells = view->selected_cells();
	assert_equal(true, rows.size() == 1 && view->rows()[rows.front()].title == "Avery",
	             "opening the name selects its person, not the page from a previous visit"sv);
	assert_equal(true, view->faces_targeted() && cells.size() == 1 && view->cells()[cells.front()].shown.path == path,
	             "the face clicked on the picture is selected on that page"sv);
	view->deactivate();
}

// docs/faces.md#9-the-faces-view: a picture naming two people is a picture of each, a name with a space in it whole,
// and each is shown by the region that names them.
static void should_list_each_person_a_picture_names()
{
	const auto folder = test_temps.next_folder("faces-people");
	const auto path = test_temps.next_path_in(folder, ".jpg");
	platform::copy_file(test_files_folder().combine_file("Test.jpg"), path, false, false);

	metadata_edits edits;
	edits.faces = face_name_edits{{{face_name_edit::kind::name, rectd{0.1, 0.1, 0.2, 0.2}, "Jane Doe", {}},
	                               {face_name_edit::kind::name, rectd{0.6, 0.1, 0.2, 0.2}, "Ann", {}}},
	                              {},
	                              core::orientation::top_left};
	files ff;
	assert_equal(true, ff.update(path, edits, {}, {}, false, {}).success(), "the names are written"sv);

	null_async_strategy as;
	const location_cache locations;
	index_state index(as, locations);
	core::index_roots roots;
	roots.folders.emplace(folder);
	index.index_roots(roots);
	index.index_folders(test_token);
	load_item(index, path, false);

	const auto model = make_faces_view_model(index, nullptr, nullptr, [] { return false; });
	assert_equal(true, model != nullptr, "the model is made"sv);
	assert_equal(2, static_cast<int>(model->people.size()), "two people"sv);

	for (const auto name : {"Jane Doe"sv, "Ann"sv})
	{
		const auto* const person = model->find_person(name);
		assert_equal(true, person != nullptr, "each named whole"sv, name);
		assert_equal(1u, person->pictures, "in the one picture"sv, name);
		assert_equal(true, person->named.size() == 1 && person->named.front().has_area(), "by their region"sv, name);
	}

	assert_near(0.6, model->find_person("Ann")->face.area.X, 1e-4, "Ann is shown by hers"sv);

	const auto before = platform::blob_from_file(path);
	auto& book = index.faces().book();
	const auto merged = book.combine("Jane Doe", "Ann", book.generation());
	assert_equal(true, merged.has_value(), "two people named only by the file can be combined locally");
	const auto combined = make_faces_view_model(index, nullptr, nullptr, [] { return false; });
	const auto* ann = combined->find_person("Ann");
	assert_equal(true, combined->people.size() == 1 && ann && ann->pictures == 1 && ann->named.size() == 2,
	             "one person keeps both regions without counting the same picture twice");
	assert_equal(true, before == platform::blob_from_file(path), "combining never edits the source picture");
}

static void should_search_for_faces_by_face_terms()
{
	// The terms are spelled one way whatever was typed, `:1` left out.
	assert_equal("face:IMG_1.jpg"sv, df::search_t::parse("face:IMG_1.jpg:1").format_terms(),
	             "the first face is the file"sv);
	assert_equal("face:IMG_1.jpg:2"sv, df::search_t::parse("@face:IMG_1.jpg:2").format_terms(), "@face: is face:"sv);
	assert_equal("@face"sv, df::search_t::parse("@face").format_terms(), "a picture with a face"sv);

	// A file name holding what the tokenizer reads otherwise - a bracket, a comma, a quote, a space - is quoted, and
	// reads back as the face it names.
	assert_equal(R"(face:"IMG_1234(1).jpg:2")"sv, df::search_for_face("IMG_1234(1).jpg", 2).format_terms(),
	             "a name with brackets is quoted"sv);
	for (const std::string_view name : {"IMG_1234(1).jpg"sv, "Bob's.jpg"sv, "Paris,France.jpg"sv, "a b.jpg"sv})
	{
		const auto search = df::search_for_face(name, 2);
		const auto read = df::search_t::parse(search.format_terms());
		assert_equal(true, read.terms() == search.terms(), "reads back as the one face it names"sv, name);
	}

	const auto here = core::folder_path("C:\\pictures");
	const auto there = core::folder_path("C:\\elsewhere");
	const auto store = std::make_shared<faces::face_store>();
	const auto add = [&](const core::folder_path folder, const std::string_view name, std::vector<faces::found_face> f)
	{
		faces::face_record r;
		r.model = faces::model_id();
		r.faces = std::move(f);
		store->add(core::file_path(folder, name), r);
	};

	add(here, "IMG_1.jpg",
	    {face_like(axis_vector(1, 0.04f, 1), {0.1, 0.1, 0.2, 0.2}),
	     face_like(axis_vector(2, 0.04f, 2), {0.5, 0.1, 0.2, 0.2})});
	add(here, "IMG_2.jpg", {face_like(axis_vector(2, 0.04f, 3), {0.3, 0.3, 0.2, 0.2})});
	add(there, "IMG_3.jpg", {face_like(axis_vector(1, 0.04f, 4), {0.3, 0.3, 0.2, 0.2})});
	add(there, "nobody.jpg", {});
	store->publish();

	const auto matches = [](const std::string_view query, const core::folder_path folder, const std::string_view name)
	{
		const auto search = df::search_t::parse(query);
		const df::search_matcher matcher(search, core::date_t(2000, 1, 1).to_days());
		df::index_file_item item;
		item.name = core::cache(name);
		item.ft = files::file_type_from_name(name);
		return matcher.match_all_terms(folder.text(), item).is_match();
	};

	faces::set_active_store(store);
	assert_equal(true, matches("face:IMG_1.jpg:2", here, "IMG_2.jpg"), "the second face's likeness"sv);
	assert_equal(false, matches("face:IMG_1.jpg:2", there, "IMG_3.jpg"), "not the first face's"sv);
	assert_equal(true, matches("face:img_1.JPG", there, "IMG_3.jpg"), "a file name whatever its case"sv);
	assert_equal(true, matches("face:IMG_1.jpg", here, "IMG_1.jpg"), "the face's own picture"sv);
	assert_equal(false, matches("face:IMG_9.jpg", here, "IMG_1.jpg"), "a file nobody has lists nothing"sv);
	assert_equal(false, matches("face:IMG_2.jpg:2", here, "IMG_2.jpg"), "nor a position past its faces"sv);
	assert_equal(true, matches("@face", here, "IMG_2.jpg"), "a picture with a face"sv);
	assert_equal(false, matches("@face", there, "nobody.jpg"), "not one with none"sv);
	assert_equal(true, matches("-@face", there, "nobody.jpg"), "which -@face finds"sv);

	faces::set_active_store(nullptr);
	assert_equal(false, matches("@face", here, "IMG_2.jpg"), "with face search off, nothing"sv);
	assert_equal(false, matches("face:IMG_1.jpg", here, "IMG_1.jpg"), "for either term"sv);
}

// docs/faces.md#8-searching: a video a search finds by its faces is marked where they are seen - the faces of the person
// a term names, those like a face: term's face, every face for @face - and nowhere a search found it by anything else.
static void should_mark_where_a_video_shows_the_faces_a_search_found()
{
	const auto folder = core::folder_path(core::windows_path_semantics ? "C:\\videos" : "/videos");
	const auto clip = folder.combine_file("clip.mp4");
	const auto still = folder.combine_file("still.jpg");

	faces::face_record video;
	video.model = faces::model_id();
	video.video = true;
	video.faces = {face_like(axis_vector(1, 0.04f, 1), {0.1, 0.1, 0.2, 0.2}),
	               face_like(axis_vector(2, 0.04f, 2), {0.5, 0.1, 0.2, 0.2})};
	video.faces[0].time_ms = 2'000;
	video.faces[0].seen = {{0, 4'000}, {30'000, 36'000}};
	video.faces[1].time_ms = 10'000;
	video.faces[1].seen = {{8'000, 31'000}};
	faces::face_record photo;
	photo.model = faces::model_id();
	photo.faces = {face_like(axis_vector(2, 0.04f, 3), {0.3, 0.3, 0.2, 0.2})};

	const auto store = std::make_shared<faces::face_store>();
	store->add(clip, video);
	store->add(still, photo);
	store->publish();

	faces::face_book book;
	const auto ann = book.create("Ann Smith");
	edit_faces(book, std::array{faces::face_ref{clip, video.faces[0].box, 0, store->signature(0)}},
	           faces::face_action::assign, ann);
	const auto old_store = faces::active_store();
	const auto old_overview = faces::active_overview();
	faces::set_active_store(store);
	faces::set_active_overview(faces::make_overview(store, no_regions, book, [] { return false; }));
	const core::scope_exit restore([&]
	{
		faces::set_active_store(old_store);
		faces::set_active_overview(old_overview);
	});

	using spans = std::vector<faces::time_span>;
	const auto found = [&clip](const std::string_view query)
	{
		return df::face_spans_found(df::search_t::parse(query), clip);
	};
	assert_equal(true, found(R"(person:"Ann Smith")") == spans{{0, 4'000}, {30'000, 36'000}},
	             "where the person a term names is seen, each part apart"sv);
	assert_equal(true, found("ann") == found(R"(person:"Ann Smith")"), "as plain text naming her finds"sv);
	assert_equal(true, found("@face") == spans{{0, 4'000}, {8'000, 36'000}},
	             "every face for @face, parts that overlap made one"sv);
	assert_equal(true, found("face:still.jpg") == spans{{8'000, 31'000}}, "a face like a picture's"sv);
	assert_equal(true, found("clip").empty() && found("-person:ann").empty(),
	             "nothing where the search found it by its name, or by a face it does not hold"sv);
	assert_equal(true, df::face_spans_found(df::search_t::parse("@face"), still).empty(), "and nothing on a picture"sv);
}

void register_faces_tests(view_state& /*state*/, test_registry& tests)
{
	tests.add("Should make the same face overview on one thread as on several"s,
	          should_make_the_same_overview_on_one_thread_as_on_several);
	tests.add("Should list unknown faces as runs of lookalikes, largest first"s,
	          should_list_unknown_faces_as_runs_of_lookalikes_largest_first);
	tests.add("Should keep unclear lookalikes out of runs"s, should_keep_unclear_lookalikes_out_of_runs);
	tests.add("Should name or hide a whole unknown group from its heading"s,
	          should_name_or_hide_a_whole_unknown_group_from_its_heading);
	tests.add("Should offer the one face of a picture to the one person it names"s,
	          should_offer_the_one_face_of_a_picture_to_the_one_person_it_names);
	tests.add("Should show a clear face as a person's portrait"s, should_show_a_clear_face_as_a_persons_portrait);
	tests.add("Should search for faces by face terms"s, should_search_for_faces_by_face_terms);
	tests.add("Should mark where a video shows the faces a search found"s,
	          should_mark_where_a_video_shows_the_faces_a_search_found);
	tests.add("Should distinguish a named face from an offer and a contradicted name"s,
	          should_distinguish_a_named_face_from_an_offer_and_a_contradicted_name);
	tests.add("Should offer only clear face matches using positive and negative evidence"s,
	          should_offer_only_clear_face_matches_using_positive_and_negative_evidence);
	tests.add("Should compare and show different looks without training on suggested faces"s,
	          should_compare_and_show_different_looks_without_training_on_suggested_faces);
	tests.add("Should offer whom a face most likely is when naming it"s,
	          should_offer_whom_a_face_most_likely_is_when_naming_it);
	tests.add("Should not offer whom a face cannot be when naming it"s,
	          should_not_offer_whom_a_face_cannot_be_when_naming_it);
	tests.add("Should list whom to name a face most likely first"s, should_list_whom_to_name_a_face_most_likely_first);
	tests.add("Should name the only face of a picture"s, should_name_the_only_face_of_a_picture);
	tests.add("Should keep faces below half similarity in Unknown"s,
	          should_keep_faces_below_half_similarity_in_unknown);
	tests.add("Should group similar Unknown faces for selection"s, should_group_similar_unknown_faces_for_selection);
	tests.add("Should pack a face record as the index keeps it"s, should_pack_a_face_record_as_the_index_keeps_it);
	tests.add("Should keep the marks on faces found again"s, should_keep_the_marks_on_faces_found_again);
	tests.add("Should read a video at the first key frame of each part"s,
	          should_read_a_video_at_the_first_key_frame_of_each_part);
	tests.add("Should make one record of a video's frames"s, should_make_one_record_of_a_videos_frames);
	tests.add("Should read a picture's faces into its record"s, should_read_a_pictures_faces_into_its_record);
	tests.add("Should recover small faces without replacing primary detections"s,
	          should_recover_small_faces_without_replacing_primary_detections);
	tests.add("Should keep faces in the store by picture"s, should_keep_faces_in_the_store_by_picture);
	tests.add("Should put away the faces of pictures the index no longer holds"s,
	          should_put_away_the_faces_of_pictures_the_index_no_longer_holds);
	tests.add("Should read a video's faces from its key frames"s, should_read_a_videos_faces_from_its_key_frames);
	tests.add("Should tell the window of face scan progress a few times a second"s,
	          should_tell_the_window_of_face_scan_progress_a_few_times_a_second);
	tests.add("Should overlap face and file scans only on seek-free storage"s,
	          should_overlap_face_and_file_scans_only_on_seek_free_storage);
	tests.add("Should undo a face placement as one book edit"s, should_undo_a_face_placement_as_one_book_edit);
	tests.add("Should combine face people and undo as one database edit"s,
	          should_combine_face_people_and_undo_as_one_database_edit);
	tests.add("Should combine file-only face names"s, should_combine_file_only_face_names);
	tests.add("Should restore combined face names from CSV"s, should_restore_combined_face_names_from_csv);
	tests.add("Should migrate an older face database without losing evidence"s,
	          should_migrate_an_older_face_database_without_losing_evidence);
	tests.add("Should round trip the face book as CSV"s, should_round_trip_the_face_book_as_csv);
	tests.add("Should refuse invalid face CSV without changing the book"s,
	          should_refuse_invalid_face_csv_without_changing_the_book);
	tests.add("Should keep the face book in its own database"s, should_keep_the_face_book_in_its_own_database);
	tests.add("Should backfill face signatures without choosing between conflicting marks"s,
	          should_backfill_face_signatures_without_choosing_between_conflicting_marks);
	tests.add("Should keep face marks with pictures as they move and change"s,
	          should_keep_marks_with_pictures_as_they_move_and_change);
	tests.add("Should refuse an incompatible face database without changing it"s,
	          should_refuse_an_incompatible_face_database);
	tests.add("Should let one window write the face book and hand it over"s,
	          should_let_one_window_write_the_face_book_and_hand_it_over);
	tests.add("Should write the face book whole after a write fails"s,
	          should_write_the_face_book_whole_after_a_write_fails);
	tests.add("Should refuse changes to a read-only face book"s, should_refuse_changes_to_a_read_only_face_book);
	tests.add("Should keep a former name when renaming someone files name in face data"s,
	          should_keep_a_former_name_when_renaming_someone_files_name);
	tests.add("Should keep writing after a face database commit fails"s,
	          should_keep_writing_after_a_face_database_commit_fails);
	tests.add("Should review, name and undo selected faces without naming neighbours"s,
	          should_review_name_and_undo_selected_faces);
	tests.add("Should keep Faces scrolled at Suggested after review"s,
	          should_keep_faces_scrolled_at_suggested_after_review);
	tests.add("Should keep Unknown in place as its faces are named"s,
	          should_keep_unknown_in_place_as_its_faces_are_named);
	tests.add("Should head only families of two or more in the Faces People list"s,
	          should_head_only_families_of_two_or_more);
	tests.add("Should collapse surnames independently in the Faces People list"s,
	          should_collapse_surnames_independently);
	tests.add("Should scroll resize and mark the Faces People list"s, should_scroll_resize_and_mark_the_people_list);
	tests.add("Should end a face name too long for the People list in an ellipsis"s,
	          should_end_a_name_too_long_for_the_people_list_in_an_ellipsis);
	tests.add("Should say when and where a face's picture was taken"s,
	          should_say_when_and_where_a_faces_picture_was_taken);
	tests.add("Should outline the hovered face on an upright preview"s,
	          should_outline_the_hovered_face_on_an_upright_preview);
	tests.add("Should say while the face suggestions are made again"s,
	          should_say_while_the_face_suggestions_are_made_again);
	tests.add("Should preview face search targets and open the picture in its folder"s,
	          should_preview_face_search_targets_and_open_the_picture_in_its_folder);
	tests.add("Should open a newly named picture face on its person page"s,
	          should_open_a_newly_named_picture_face_on_its_person_page);
	tests.add("Should list each person a picture names, each shown by their face region"s,
	          should_list_each_person_a_picture_names);
	tests.add("Should embed a face alike whatever the decode's orientation"s,
	          should_embed_a_face_alike_whatever_the_decodes_orientation);
	tests.add("Should quantize face hashes closely"s, should_quantize_face_hashes_closely);
	tests.add("Should measure how square-on a face is"s, should_measure_how_square_on_a_face_is);
	tests.add("Should keep a face tile inside its picture"s, should_keep_a_face_tile_inside_its_picture);
	tests.add("Should turn face regions upright and back"s, should_turn_face_regions_upright_and_back);
	tests.add("Should read where a face region is by the shape it was measured on"s,
	          should_read_where_a_face_region_is_by_the_shape_it_was_measured_on);
	tests.add("Should pack face regions as the file states them"s, should_pack_face_regions_as_the_file_states_them);
	tests.add("Should read the people a picture's metadata names and their face regions"s,
	          should_read_the_people_a_pictures_metadata_names);
	tests.add("Should detect the face in a portrait"s, should_detect_the_face_in_a_portrait);
	tests.add("Should detect faces whatever the picture's orientation"s,
	          should_detect_faces_whatever_the_pictures_orientation);
	tests.add("Should detect faces alike in planar and packed decodes"s,
	          should_detect_faces_alike_in_planar_and_packed_decodes);
	tests.add("Should detect faces in a picture larger than the detector reads"s,
	          should_detect_faces_in_a_picture_larger_than_the_detector_reads);
	tests.add("Should detect no face where there is none"s, should_detect_no_face_where_there_is_none);
}
