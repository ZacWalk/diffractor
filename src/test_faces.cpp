// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Tests for face detection (model_faces) - the luminance the detector reads, that plane turned
// upright, the stable order, and the detector itself over libfacedetection's sample photographs: held
// to libfacedetection's own entry point, in every orientation, from packed and planar decodes, reduced,
// and over pictures with nobody in them. docs/faces.md owns the behaviour.

#include "pch.h"

#include "files.h"
#include "model_faces.h"
#include "test.h"
#include "test_fixtures.h"
#include "test_runner.h"

#include <facedetect_hw.h>

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

static void should_reduce_luminance_at_odd_extents_for_detecting_faces()
{
	const auto packed = std::make_shared<core::surface>();
	const auto planar = std::make_shared<core::surface>();
	assert_equal(true, packed->alloc(64, 32, core::texture_format::ARGB) != nullptr, "packed scratch"sv);
	assert_equal(true, planar->alloc(64, 32, core::texture_format::NV12) != nullptr, "planar scratch"sv);
	planar->color_space(core::color_space::rec601_full);
	auto seed = 0x9e3779b9u;
	for (auto y = 0; y < 32; ++y)
	{
		auto* color = packed->pixels_line(y);
		auto* luma = planar->pixels() + planar->stride() * y;
		for (auto x = 0; x < 64; ++x)
		{
			seed = seed * 1664525u + 1013904223u;
			const auto value = static_cast<uint8_t>(seed >> 24);
			luma[x] = value;
			color[x * 4] = color[x * 4 + 1] = color[x * 4 + 2] = value;
			color[x * 4 + 3] = 255;
		}
	}

	// Odd extents are the case an NV12 surface cannot hold, so the planar path reduces its luma plane
	// itself - with the shared scaler's weights, or one picture would reach the detector two ways.
	for (const auto extent : {sizei{16, 8}, sizei{15, 7}, sizei{64, 32}, sizei{31, 32}})
	{
		core::surface_ptr scratch;
		std::vector<uint8_t> packed_plane;
		std::vector<uint8_t> planar_plane;
		assert_equal(true, faces::reduce_to_luma(packed, extent, scratch, packed_plane), "packed reduction"sv);
		assert_equal(true, faces::reduce_to_luma(planar, extent, scratch, planar_plane), "planar reduction"sv);
		assert_equal(extent.cx * extent.cy, static_cast<int>(planar_plane.size()), "exact detector extent"sv);
		assert_equal(true, packed_plane == planar_plane, "the luma-only path agrees with the shared scaler"sv);
	}

	core::surface_ptr scratch;
	std::vector<uint8_t> result;
	assert_equal(false, faces::reduce_to_luma(planar, {65, 32}, scratch, result), "luma input is not enlarged"sv);
}

static void should_normalize_luminance_for_detecting_faces()
{
	const auto packed = std::make_shared<core::surface>();
	const auto planar = std::make_shared<core::surface>();
	assert_equal(true, packed->alloc({4, 2}, core::texture_format::RGB) != nullptr, "packed luma fixture"sv);
	assert_equal(true, planar->alloc({4, 2}, core::texture_format::NV12) != nullptr, "planar luma fixture"sv);
	core::surface_ptr scratch;
	std::vector<uint8_t> packed_luma;
	std::vector<uint8_t> planar_luma;

	for (const auto value : {0, 128, 255})
	{
		for (auto row = 0; row < 2; ++row) std::fill_n(packed->pixels_line(row), 16, static_cast<uint8_t>(value));
		assert_equal(true, faces::reduce_to_luma(packed, {2, 2}, scratch, packed_luma), "packed reduction"sv);
		assert_equal(value, static_cast<int>(packed_luma[0]), "packed grey keeps its full-range luminance"sv);

		// A WebP decodes limited range and a JPEG full range: the detector must see one brightness for
		// one picture, whichever it came as.
		for (const auto limited : {false, true})
		{
			planar->color_space(limited ? core::color_space::rec601_limited : core::color_space::rec601_full);
			const auto sample = limited ? 16 + (value * 219 + 127) / 255 : value;
			for (auto row = 0; row < 2; ++row) std::fill_n(planar->pixels_line(row), 4, static_cast<uint8_t>(sample));
			assert_equal(true, faces::reduce_to_luma(planar, {2, 2}, scratch, planar_luma), "planar reduction"sv);
			assert_equal(true, packed_luma == planar_luma, "decoder range does not change detector brightness"sv);
		}
	}
}

static void should_turn_luminance_upright_for_detecting_faces()
{
	// Stored as
	//   1 2 3
	//   4 5 6
	// and shown as each orientation's EXIF meaning says: right_top's stored first row is the picture's
	// right-hand side, so it is shown turned a quarter clockwise.
	const std::vector<uint8_t> stored{1, 2, 3, 4, 5, 6};

	struct expectation
	{
		core::orientation orientation;
		sizei extent;
		std::vector<uint8_t> shown;
	};

	const expectation expectations[]{
		{core::orientation::top_left, {3, 2}, {1, 2, 3, 4, 5, 6}},
		{core::orientation::top_right, {3, 2}, {3, 2, 1, 6, 5, 4}},
		{core::orientation::bottom_right, {3, 2}, {6, 5, 4, 3, 2, 1}},
		{core::orientation::bottom_left, {3, 2}, {4, 5, 6, 1, 2, 3}},
		{core::orientation::left_top, {2, 3}, {1, 4, 2, 5, 3, 6}},
		{core::orientation::right_top, {2, 3}, {4, 1, 5, 2, 6, 3}},
		{core::orientation::right_bottom, {2, 3}, {6, 3, 5, 2, 4, 1}},
		{core::orientation::left_bottom, {2, 3}, {3, 6, 2, 5, 1, 4}},
	};

	for (const auto& e : expectations)
	{
		std::vector<uint8_t> shown;
		assert_equal(true, faces::orient_plane(stored, {3, 2}, e.orientation, shown), "the plane turns"sv);
		const auto extent = faces::upright_extent({3, 2}, e.orientation);
		assert_equal(e.extent.cx, extent.cx, "upright width"sv);
		assert_equal(e.extent.cy, extent.cy, "upright height"sv);
		assert_equal(true, shown == e.shown, "each pixel lands where the picture shows it"sv);
	}

	std::vector<uint8_t> refused;
	assert_equal(false, faces::orient_plane(std::span(stored).first(5), {3, 2}, core::orientation::right_top, refused),
	             "a plane shorter than its extent is refused"sv);

	// The same turn the application shows a picture with. A detector that turned its input any other
	// way would put the boxes somewhere other than the faces on screen.
	const auto source = std::make_shared<core::surface>();
	assert_equal(true, source->alloc({5, 3}, core::texture_format::ARGB) != nullptr, "packed fixture"sv);
	for (auto y = 0; y < 3; ++y)
	{
		auto* const line = source->pixels_line(y);
		for (auto x = 0; x < 5; ++x)
		{
			const auto value = static_cast<uint8_t>(10 + y * 5 + x);
			line[x * 4] = line[x * 4 + 1] = line[x * 4 + 2] = value;
			line[x * 4 + 3] = 255;
		}
	}

	core::surface_ptr scratch;
	std::vector<uint8_t> source_luma;
	assert_equal(true, faces::reduce_to_luma(source, {5, 3}, scratch, source_luma), "stored luminance"sv);

	for (auto o = static_cast<int>(core::orientation::top_right); o <= static_cast<int>(core::orientation::left_bottom);
	     ++o)
	{
		const auto orientation = static_cast<core::orientation>(o);
		const core::const_surface_ptr displayed = source->transform(core::to_simple_transform(orientation));
		assert_equal(true, core::is_valid(displayed), "the application turns the picture"sv);

		std::vector<uint8_t> displayed_luma;
		assert_equal(true, faces::reduce_to_luma(displayed, displayed->dimensions(), scratch, displayed_luma),
		             "displayed luminance"sv);
		std::vector<uint8_t> turned;
		assert_equal(true, faces::orient_plane(source_luma, {5, 3}, orientation, turned), "the plane turns"sv);
		assert_equal(true, turned == displayed_luma, "the detector's plane turns as the picture is shown"sv);
	}
}

static void should_order_detected_faces_stably()
{
	const auto face = [](const double x, const double y, const double extent)
	{
		faces::face f;
		f.bounds = rectd{x, y, extent, extent};
		return f;
	};

	std::vector<faces::face> detected{
		face(400, 100, 80),
		face(100, 300, 60),
		face(100, 100, 50),
		face(100, 100, 90),
	};

	auto ordered = detected;
	faces::order_faces(ordered);

	assert_near(100.0, ordered[0].bounds.X, 1e-9, "reading order runs left to right"sv);
	assert_near(100.0, ordered[0].bounds.Y, 1e-9, "then top to bottom"sv);
	assert_near(90.0, ordered[0].bounds.Width, 1e-9, "and the larger of two boxes in one place leads"sv);
	assert_near(50.0, ordered[1].bounds.Width, 1e-9, "followed by the smaller"sv);
	assert_near(300.0, ordered[2].bounds.Y, 1e-9, "then the face below them"sv);
	assert_near(400.0, ordered[3].bounds.X, 1e-9, "and the one further right comes last"sv);

	// The detector's output order follows its scores, so the answer must not depend on it.
	std::ranges::reverse(detected);
	auto reordered = detected;
	faces::order_faces(reordered);

	for (auto i = 0_z; i < ordered.size(); ++i)
	{
		assert_near(ordered[i].bounds.X, reordered[i].bounds.X, 1e-9, "the order does not depend on arrival order"sv);
		assert_near(ordered[i].bounds.Y, reordered[i].bounds.Y, 1e-9, "the order does not depend on arrival order"sv);
		assert_near(ordered[i].bounds.Width, reordered[i].bounds.Width, 1e-9,
		            "the order does not depend on arrival order"sv);
	}
}

static void should_detect_the_face_in_a_portrait()
{
	const auto portrait = decode_picture(face_sample("test.jpg"));
	assert_equal(true, core::is_valid(portrait), "libfacedetection's portrait decodes"sv);

	const auto detector = faces::create_face_detector();
	const auto found = faces::find_faces(*detector, portrait);
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
	assert_equal(0, static_cast<int>(faces::find_faces(*detector, portrait, sure).faces.size()),
	             "a face below the confidence floor is not reported"sv);

	faces::tuning large;
	large.min_face_edge_fraction = face.bounds.Width / found.extent.cx + 0.05;
	assert_equal(0, static_cast<int>(faces::find_faces(*detector, portrait, large).faces.size()),
	             "a face below the size floor is not reported"sv);
}

static void should_detect_every_face_in_a_group_photograph()
{
	const auto group = decode_picture(face_sample("test2.jpg"));
	assert_equal(true, core::is_valid(group), "libfacedetection's group photograph decodes"sv);

	const auto detector = faces::create_face_detector();
	const auto found = faces::find_faces(*detector, group);

	// Eleven faces, each found with confidence near 90. The next candidate libfacedetection itself
	// reports scores near 30, so neither side of the floor is close.
	assert_equal(11, static_cast<int>(found.faces.size()), "every face in the photograph"sv);

	for (auto i = 0_z; i < found.faces.size(); ++i)
	{
		const auto& face = found.faces[i];
		assert_equal(true, face.confidence >= 80, "each face is found with confidence"sv);
		assert_equal(true, rectd(0, 0, found.extent.cx, found.extent.cy).contains(face.bounds),
		             "each face lies in the picture"sv);

		for (auto j = i + 1; j < found.faces.size(); ++j)
		{
			assert_equal(true, box_overlap(face.bounds, found.faces[j].bounds) < 0.2, "one face is reported once"sv);
		}

		if (i > 0)
		{
			assert_equal(true, core::round(found.faces[i - 1].bounds.X) <= core::round(face.bounds.X),
			             "faces are listed left to right"sv);
		}
	}
}

// Diffractor feeds the network luminance where libfacedetection's own entry point feeds colour, and
// selects with its own code. Neither may change what is found: every confident face one reports, the
// other reports too, in the same place.
static void should_detect_the_faces_libfacedetection_detects()
{
	const auto detector = faces::create_face_detector();

	for (const auto* const name : {"test.jpg", "test2.jpg"})
	{
		const auto picture = decode_picture(face_sample(name));
		assert_equal(true, core::is_valid(picture) && core::is_packed(picture->format()), name,
		             "the sample decodes packed"sv);

		const auto extent = picture->dimensions();
		std::vector<uint8_t> bgr(static_cast<size_t>(extent.cx) * extent.cy * 3);

		for (auto y = 0; y < extent.cy; ++y)
		{
			const auto* const line = picture->pixels_line(y);
			auto* const out = bgr.data() + static_cast<size_t>(y) * extent.cx * 3;
			for (auto x = 0; x < extent.cx; ++x) std::copy_n(line + x * 4, 3, out + x * 3);
		}

		std::vector<unsigned char> buffer(FACEDETECTION_HW_RESULT_BUFFER_SIZE);
		const auto* const result = facedetect_hw_cnn(buffer.data(), bgr.data(), extent.cx, extent.cy, extent.cx * 3);
		assert_equal(true, result != nullptr, name, "libfacedetection runs"sv);

		struct reference
		{
			int confidence;
			rectd bounds;
		};

		std::vector<reference> references;
		for (auto i = 0; i < result[0]; ++i)
		{
			const auto* const r = std::bit_cast<const short*>(buffer.data() + 4) +
			                      FACEDETECTION_HW_RESULT_STRIDE_SHORTS * i;
			references.push_back({r[0], rectd(r[1], r[2], r[3], r[4])});
		}

		const auto found = faces::find_faces(*detector, picture);
		const auto matching = [](const rectd box, const auto& candidates)
		{
			return std::ranges::find_if(candidates,
			                            [box](const auto& c) { return box_overlap(box, c.bounds) >= 0.75; });
		};

		for (const auto& r : references)
		{
			if (r.confidence < 80) continue;
			const auto match = matching(r.bounds, found.faces);
			assert_equal(true, match != found.faces.end(), name, "a face libfacedetection is sure of is found"sv);
			assert_near(r.confidence, match->confidence, 5.0, name, "with much the same confidence"sv);
		}

		for (const auto& f : found.faces)
		{
			assert_equal(true, matching(f.bounds, references) != references.end(), name,
			             "every face found is one libfacedetection finds"sv);
		}
	}
}

// Most formats decode in the file's own orientation, so a portrait photograph reaches the detector on
// its side. The faces it reports are those of the picture as it is shown, whichever way it was stored.
static void should_detect_faces_whatever_the_pictures_orientation()
{
	const auto upright = decode_picture(face_sample("test.jpg"));
	assert_equal(true, core::is_valid(upright), "libfacedetection's portrait decodes"sv);

	const auto detector = faces::create_face_detector();
	const auto expected = faces::find_faces(*detector, upright);
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

		const auto found = faces::find_faces(*detector, stored);
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

	const auto detector = faces::create_face_detector();
	const auto from_packed = faces::find_faces(*detector, packed);
	const auto from_planar = faces::find_faces(*detector, planar);

	// The planar decode's luminance comes straight from its Y plane and the packed one's from colour
	// converted back, so they differ by rounding and no more.
	assert_same_faces(from_packed, from_planar, 0.5, "one picture, decoded either way"sv);
}

static void should_detect_faces_in_a_picture_larger_than_the_detector_reads()
{
	const auto group = decode_picture(face_sample("test2.jpg"));
	assert_equal(true, core::is_valid(group), "libfacedetection's group photograph decodes"sv);

	const auto detector = faces::create_face_detector();
	const auto full = faces::find_faces(*detector, group);

	// The detector reads a plane whose long edge is 320 here, so every box it finds is scaled back to
	// the picture's own dimensions.
	faces::tuning bounded;
	bounded.detector_edge = 320;
	const auto reduced = faces::find_faces(*detector, group, bounded);
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
	assert_same_faces(reduced, faces::find_faces(*detector, stored, bounded), 1.5,
	                  "a reduced picture stored on its side"sv);
}

static void should_detect_no_face_where_there_is_none()
{
	const auto detector = faces::create_face_detector();

	// A city's windows and a dog looking at the camera, decoded no larger than the detector reads.
	for (const auto* const name : {"IMG_9340.JPG", "IMG_0096.JPG"})
	{
		const auto picture = decode_picture(test_files_folder().combine_file(name), false,
		                                    {faces::defaults().detector_edge, faces::defaults().detector_edge});
		assert_equal(true, core::is_valid(picture), name, "the fixture decodes"sv);
		assert_equal(0, static_cast<int>(faces::find_faces(*detector, picture).faces.size()), name,
		             "nobody is in the picture"sv);
	}

	const auto gradient = std::make_shared<core::surface>();
	assert_equal(true, gradient->alloc({320, 240}, core::texture_format::ARGB) != nullptr, "gradient"sv);
	for (auto y = 0; y < 240; ++y)
	{
		auto* const line = gradient->pixels_line(y);
		for (auto x = 0; x < 320; ++x)
		{
			line[x * 4] = static_cast<uint8_t>(x * 255 / 319);
			line[x * 4 + 1] = static_cast<uint8_t>(y * 255 / 239);
			line[x * 4 + 2] = static_cast<uint8_t>((x + y) * 255 / 558);
			line[x * 4 + 3] = 255;
		}
	}
	assert_equal(0, static_cast<int>(faces::find_faces(*detector, gradient).faces.size()), "a gradient has no face"sv);

	const auto ten_bit = std::make_shared<core::surface>();
	assert_equal(true, ten_bit->alloc({64, 64}, core::texture_format::P010) != nullptr, "P010 surface"sv);
	assert_equal(0, static_cast<int>(faces::find_faces(*detector, ten_bit).faces.size()),
	             "a format the detector does not read finds no face"sv);
	assert_equal(0, static_cast<int>(faces::find_faces(*detector, nullptr).faces.size()), "no picture, no face"sv);
}

void register_faces_tests(view_state& /*state*/, test_registry& tests)
{
	tests.add("Should reduce luminance at odd extents for detecting faces"s,
	          should_reduce_luminance_at_odd_extents_for_detecting_faces);
	tests.add("Should normalize luminance for detecting faces"s, should_normalize_luminance_for_detecting_faces);
	tests.add("Should turn luminance upright for detecting faces"s, should_turn_luminance_upright_for_detecting_faces);
	tests.add("Should order detected faces stably"s, should_order_detected_faces_stably);
	tests.add("Should detect the face in a portrait"s, should_detect_the_face_in_a_portrait);
	tests.add("Should detect every face in a group photograph"s, should_detect_every_face_in_a_group_photograph);
	tests.add("Should detect the faces libfacedetection detects"s, should_detect_the_faces_libfacedetection_detects);
	tests.add("Should detect faces whatever the picture's orientation"s,
	          should_detect_faces_whatever_the_pictures_orientation);
	tests.add("Should detect faces alike in planar and packed decodes"s,
	          should_detect_faces_alike_in_planar_and_packed_decodes);
	tests.add("Should detect faces in a picture larger than the detector reads"s,
	          should_detect_faces_in_a_picture_larger_than_the_detector_reads);
	tests.add("Should detect no face where there is none"s, should_detect_no_face_where_there_is_none);
}
