// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Media metadata write tests and the image editing model. Verifies metadata, rating, label,
// tag and location updates across containers and sidecars, Windows shell tag interop, the edit
// operations (perspective, document detection, crop, temperature, preview, ICC/XMP round-trip), a JPEG
// repacked as a JPEG XL that keeps giving it back, converted or turned, and the metadata a lossless audio
// or video edit carries through the staged write.

#include "pch.h"
#include "test_fixtures.h"
#include "av_format.h"
#include "metadata.h"
#include "view_edit.h"
#include "app_util.h"
#include "app_commands.h"

#include "libfiles/encode.h"
#include "libfiles/media.h"
#include "libfiles/scan.h"
#include "libfiles/write.h"

#include <set>

// Writing metadata updates xmp:ModifyDate, and on a file that carried no XMP the toolkit can add an
// xmp:CreateDate too, so neither is stable across a write. The capture time is: a write that moves
// it has destroyed when the photograph was taken (#184, #192). Assert that, then let the rest go.
static void reconcile_write_mutable_dates(prop::item_metadata& expected, const prop::item_metadata& actual,
                                          const std::string_view message)
{
	assert_equal(expected.dates.original(), actual.dates.original(), "date original survives a write", message);
	expected.dates = actual.dates;
}

#ifdef _WIN32
static uint32_t read_be32(const uint8_t* p)
{
	return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
	       p[3];
}

static core::blob find_xtra_entry(const core::file_path path, const std::string_view wanted_name)
{
	const auto data = platform::blob_from_file(path);
	for (size_t pos = 0; pos + 12 <= data.size(); ++pos)
	{
		const auto entry_size = read_be32(data.data() + pos);
		const auto name_size = read_be32(data.data() + pos + 4);
		if (entry_size < 12 || entry_size > data.size() - pos || name_size > entry_size - 12) continue;
		const std::string_view name(reinterpret_cast<const char*>(data.data() + pos + 8), name_size);
		if (name == wanted_name)
		{
			return {data.begin() + static_cast<ptrdiff_t>(pos),
			        data.begin() + static_cast<ptrdiff_t>(pos + entry_size)};
		}
	}
	return {};
}
#endif

static core::cached make_unique_tags(tag_set tags1, const tag_set& tags2)
{
	tags1.add(tags2);
	tags1.make_unique();
	return core::cache(tags1.to_string());
}

static void should_apply_perspective_correction()
{
	const auto source = std::make_shared<core::surface>();
	source->alloc(16, 12, core::texture_format::RGB);

	for (auto y = 0; y < 12; ++y)
	{
		for (auto x = 0; x < 16; ++x)
		{
			source->set_pixel(x, y, core::rgba(x * 16, y * 20, 64));
		}
	}

	image_edits edits;
	edits.perspective_horizontal(0.4);
	edits.perspective_vertical(-0.25);
	const auto corrected = apply_image_edits(source, edits);

	assert_equal(true, source->dimensions() == corrected->dimensions(), "perspective dimensions");
	assert_equal(true, source->pixel_difference(corrected) == core::pixel_difference_result::not_equal,
	             "perspective changes pixels");

	edits.crop_bounds(quadd(source->dimensions()));
	const auto valid = edits.perspective_bounds(source->dimensions());
	const auto crop = edits.effective_crop_bounds(source->dimensions());
	const auto inside = [](const quadd& polygon, const pointd point)
	{
		auto sign = 0.0;
		for (auto edge = 0; edge < 4; ++edge)
		{
			const auto start = polygon[edge];
			const auto end = polygon[(edge + 1) % 4];
			const auto cross = (end.X - start.X) * (point.Y - start.Y) - (end.Y - start.Y) * (point.X - start.X);
			if (edge == 0) sign = cross;
			else if (sign * cross < -0.0001) return false;
		}
		return true;
	};
	assert_equal(true, crop.actual_extent().Width < source->width(), "perspective crop narrows invalid edges");
	for (auto corner = 0; corner < 4; ++corner)
	{
		assert_equal(true, inside(valid, crop[corner]), "perspective crop corner is valid");
	}

	const auto rotated_bounds = quadd(source->dimensions()).rotate(20, quadd(source->dimensions()).center_point());
	edits.crop_bounds(rotated_bounds);
	const auto rotated_valid = edits.perspective_bounds(source->dimensions());
	const auto rotated_crop = edits.effective_crop_bounds(source->dimensions());
	for (auto corner = 0; corner < 4; ++corner)
	{
		assert_equal(true, inside(rotated_valid, rotated_crop[corner]), "rotated perspective crop corner is valid");
	}

	edits.perspective_horizontal(0);
	edits.perspective_vertical(0);
	edits.crop_bounds({});
	assert_equal(true, apply_image_edits(source, edits).get() == source.get(), "zero perspective preserves surface");
}

static void should_fit_document_correction_to_controls()
{
	constexpr sizei extent(400, 300);
	constexpr auto horizontal = 0.30;
	constexpr auto vertical = -0.18;

	// A page filling this rectangle once corrected must have been photographed at these corners.
	const quadd page_in_output(60.0, 50.0, 340.0, 250.0);
	std::array<pointd, 4> corners{};

	for (auto index = 0; index < 4; ++index)
	{
		const auto nx = page_in_output[index].X / extent.cx - 0.5;
		const auto ny = page_in_output[index].Y / extent.cy - 0.5;
		const auto denominator = 1.0 + horizontal * nx + vertical * ny;
		corners[index] = {(0.5 + nx / denominator) * extent.cx, (0.5 + ny / denominator) * extent.cy};
	}

	const auto correction = fit_document_correction(corners, extent, core::orientation::top_left);
	assert_equal(true, static_cast<bool>(correction), "document correction fits");
	assert_equal(true, std::abs(correction.perspective_horizontal - horizontal) < 0.01,
	             "recovers horizontal perspective");
	assert_equal(true, std::abs(correction.perspective_vertical - vertical) < 0.01, "recovers vertical perspective");
	assert_equal(true, std::abs(correction.straighten) < 0.5, "rectified page needs no straighten");

	// The crop stays a rectangle the user can adjust, and needs no clipping because it is already
	// inside the warped image.
	image_edits edits;
	edits.perspective_horizontal(correction.perspective_horizontal);
	edits.perspective_vertical(correction.perspective_vertical);
	edits.crop_bounds(correction.crop);
	assert_equal(true, std::abs(edits.crop_bounds().angle() - correction.straighten) < 0.5,
	             "crop angle matches straighten");

	const auto effective = edits.effective_crop_bounds(extent);

	for (auto corner = 0; corner < 4; ++corner)
	{
		assert_equal(true, correction.crop[corner].dist(page_in_output[corner]) < 2.0, "crop matches the page");
		assert_equal(true, correction.crop[corner].dist(effective[corner]) < 0.5, "crop needs no clipping");
	}
}

static void should_detect_only_clear_document_regions()
{
	const auto document = std::make_shared<core::surface>();
	document->alloc(160, 120, core::texture_format::RGB);
	constexpr std::array<pointd, 4> corners = {pointd(24, 18), pointd(140, 12), pointd(132, 108), pointd(18, 100)};
	for (auto y = 0; y < 120; ++y)
	{
		for (auto x = 0; x < 160; ++x)
		{
			bool inside = true;
			for (auto edge = 0; edge < 4; ++edge)
			{
				const auto& a = corners[edge];
				const auto& b = corners[(edge + 1) % 4];
				if ((b.X - a.X) * (y + 0.5 - a.Y) - (b.Y - a.Y) * (x + 0.5 - a.X) < 0)
				{
					inside = false;
					break;
				}
			}
			document->set_pixel(x, y, inside ? core::rgba(238, 236, 232) : core::rgba(40, 55, 70));
		}
	}

	const auto detected = detect_document(document, document->dimensions());
	assert_equal(true, static_cast<bool>(detected), "detects clear document");
	assert_equal(true, detected.extent.cx > 100 && detected.extent.cy > 80, "document extent is plausible");

	const auto ambiguous = std::make_shared<core::surface>();
	ambiguous->alloc(160, 120, core::texture_format::RGB);
	for (auto y = 0; y < 120; ++y)
		for (auto x = 0; x < 160; ++x) ambiguous->set_pixel(x, y, core::rgba(220, 220, 220));
	assert_equal(true, !detect_document(ambiguous, ambiguous->dimensions()), "rejects borderless bright image");
}

static void should_detect_photographed_document()
{
	files loader;
	const auto loaded = loader.load(test_files_folder().combine("excluded1").combine_file("document.png"), false);
	const auto surface = loaded.to_surface({768, 768});
	const auto detected = detect_document(surface, loaded.dimensions());

	assert_equal(true, static_cast<bool>(detected), "detects photographed document");
	assert_equal(true,
	             detected.extent.cx > loaded.dimensions().cx / 2 && detected.extent.cy > loaded.dimensions().cy / 2 &&
	                 detected.extent.cx <= loaded.dimensions().cx && detected.extent.cy <= loaded.dimensions().cy,
	             "photographed document extent is plausible");
	assert_equal(true, detected.extent.cy > detected.extent.cx, "photographed document remains portrait");
}

static void should_detect_document_at_image_edge()
{
	const auto document = std::make_shared<core::surface>();
	document->alloc(160, 120, core::texture_format::RGB);
	for (auto y = 0; y < 120; ++y)
	{
		for (auto x = 0; x < 160; ++x)
		{
			const auto inside = x < 130 && y < 100;
			document->set_pixel(x, y, inside ? core::rgba(238, 236, 232) : core::rgba(40, 55, 70));
		}
	}

	const auto detected = detect_document(document, document->dimensions());
	assert_equal(true, static_cast<bool>(detected), "detects document touching image edge");
}

static void should_detect_low_contrast_document()
{
	const auto document = std::make_shared<core::surface>();
	document->alloc(240, 180, core::texture_format::RGB);
	constexpr std::array<pointd, 4> corners = {pointd(34, 22), pointd(210, 30), pointd(202, 160), pointd(26, 152)};

	for (auto y = 0; y < 180; ++y)
	{
		for (auto x = 0; x < 240; ++x)
		{
			bool inside = true;
			for (auto edge = 0; edge < 4; ++edge)
			{
				const auto& a = corners[edge];
				const auto& b = corners[(edge + 1) % 4];
				if ((b.X - a.X) * (y + 0.5 - a.Y) - (b.Y - a.Y) * (x + 0.5 - a.X) < 0)
				{
					inside = false;
					break;
				}
			}
			// Lighting ramp overlaps page and surround tones, so no global threshold separates them.
			const auto level = core::round((inside ? 148.0 : 120.0) + 70.0 * x / 240.0);
			document->set_pixel(x, y, core::rgba(level, level, level));
		}
	}

	const auto detected = detect_document(document, document->dimensions());
	assert_equal(true, static_cast<bool>(detected), "detects low contrast document");
	assert_equal(true, detected.extent.cx > 140 && detected.extent.cx < 210, "low contrast document width");
	assert_equal(true, detected.extent.cy > 100 && detected.extent.cy < 160, "low contrast document height");
}

static void should_crop_without_resampling()
{
	const auto source = std::make_shared<core::surface>();
	source->alloc(16, 12, core::texture_format::RGB);

	for (auto y = 0; y < 12; ++y)
	{
		for (auto x = 0; x < 16; ++x)
		{
			// Alternating extremes, so any interpolation shows up immediately as a mid tone.
			source->set_pixel(x, y, (x + y) & 1 ? core::rgba(255, 255, 255) : core::rgba(0, 0, 0));
		}
	}

	// A crop dragged in the view lands on fractional source pixels.
	image_edits edits;
	edits.crop_bounds(quadd(rectd(3.4, 2.6, 8.0, 6.0)));

	const auto cropped = apply_image_edits(source, edits);
	assert_equal(true, cropped->dimensions() == sizei(8, 6), "crop extent");

	for (auto y = 0u; y < cropped->height(); ++y)
	{
		for (auto x = 0u; x < cropped->width(); ++x)
		{
			assert_equal(source->get_pixel(3 + x, 3 + y), cropped->get_pixel(x, y), "crop copies source pixels");
		}
	}
}

static void should_apply_temperature_and_tint()
{
	const auto source = std::make_shared<core::surface>();
	source->alloc(1, 1, core::texture_format::RGB);
	source->set_pixel(0, 0, core::rgba(128, 128, 128));

	image_edits edits;
	edits.temperature(0.5);
	edits.tint(0.25);
	const auto adjusted = apply_image_edits(source, edits)->get_pixel(0, 0);
	const auto adjusted_red = core::get_b(adjusted);
	const auto adjusted_green = core::get_g(adjusted);
	const auto adjusted_blue = core::get_r(adjusted);

	assert_equal(true, adjusted_red > adjusted_blue, "temperature warms neutral pixel");
	assert_equal(true, adjusted_green < adjusted_red, "positive tint reduces green");
}

// Auto colour reads a cast from the near-neutral pixels and asks for the white balance that removes it. It
// once read the first byte - blue, in a packed surface - as red, and so warmed a warm picture further.
static void should_auto_colour_against_a_cast()
{
	// A grey ramp under a cast, written byte by byte so the test names each channel itself.
	const auto cast_ramp = [](const double red, const double green, const double blue)
	{
		const auto ramp = std::make_shared<core::surface>();
		ramp->alloc(176, 1, core::texture_format::RGB);
		auto* const row = ramp->pixels_line(0);

		for (auto x = 0; x < 176; ++x)
		{
			const auto level = 40.0 + x;
			row[x * 4 + 0] = static_cast<uint8_t>(std::min(255.0, std::round(level * blue)));
			row[x * 4 + 1] = static_cast<uint8_t>(std::min(255.0, std::round(level * green)));
			row[x * 4 + 2] = static_cast<uint8_t>(std::min(255.0, std::round(level * red)));
			row[x * 4 + 3] = 255;
		}

		return core::const_surface_ptr(ramp);
	};

	const auto red_over_blue = [](const core::const_surface_ptr& s)
	{
		double red = 0;
		double blue = 0;
		for (auto x = 0u; x < s->width(); ++x)
		{
			blue += s->pixels_line(0)[x * 4 + 0];
			red += s->pixels_line(0)[x * 4 + 2];
		}
		return red / blue;
	};

	const auto warm = cast_ramp(1.12, 1.0, 0.85);
	const auto settings = analyze_auto_color(warm);
	assert_equal(true, settings.has_value(), "a warm picture is measured");
	if (!settings) return;
	assert_equal(true, settings->temperature < -0.2, "a warm cast asks for a cooler temperature");

	render::color_adjust adjust;
	adjust.color_params(settings->vibrance, 0, settings->darks, settings->midtones, settings->lights,
	                    settings->contrast, settings->brightness, settings->temperature, settings->tint);
	const auto corrected = std::make_shared<core::surface>();
	corrected->alloc(warm->dimensions(), warm->format());
	adjust.apply(warm, corrected->pixels(), corrected->stride(), {});
	assert_equal(true, red_over_blue(corrected) < red_over_blue(warm) - 0.1, "and applying it takes the cast out");

	const auto cool = analyze_auto_color(cast_ramp(0.85, 1.0, 1.12));
	assert_equal(true, cool && cool->temperature > 0.2, "a cool cast asks for a warmer temperature");

	const auto green = analyze_auto_color(cast_ramp(0.95, 1.1, 0.95));
	assert_equal(true, green && green->tint > 0.1, "a green cast asks for tint towards magenta");

	const auto neutral = analyze_auto_color(cast_ramp(1.0, 1.0, 1.0));
	assert_equal(true, neutral && std::abs(neutral->temperature) < 0.01 && std::abs(neutral->tint) < 0.01,
	             "a neutral picture needs no white balance");
}

static void should_initialize_edit_rotation_from_orientation()
{
	constexpr sizei landscape(120, 80);
	const auto top_left = edit_view_state::initial_crop(landscape, core::orientation::top_left);
	const auto right_top = edit_view_state::initial_crop(landscape, core::orientation::right_top);
	const auto bottom_right = edit_view_state::initial_crop(landscape, core::orientation::bottom_right);
	const auto left_bottom = edit_view_state::initial_crop(landscape, core::orientation::left_bottom);

	assert_equal(0.0, top_left.angle(), "top-left rotation");
	assert_equal(-90.0, right_top.angle(), "right-top rotation");
	assert_equal(180.0, std::abs(bottom_right.angle()), "bottom-right rotation");
	assert_equal(90.0, left_bottom.angle(), "left-bottom rotation");
	assert_equal(true, right_top.actual_extent().Height > right_top.actual_extent().Width,
	             "right-top orientation is portrait");
	assert_equal(true, left_bottom.actual_extent().Height > left_bottom.actual_extent().Width,
	             "left-bottom orientation is portrait");

	const auto source = std::make_shared<core::surface>();
	source->alloc(3, 2, core::texture_format::RGB);
	for (auto y = 0; y < 2; ++y)
	{
		for (auto x = 0; x < 3; ++x)
		{
			source->set_pixel(x, y, core::rgba(10 + x + y * 3, 0, 0));
		}
	}

	image_edits edits;
	edits.crop_bounds(edit_view_state::initial_crop(source->dimensions(), core::orientation::right_top));
	const auto edited = apply_image_edits(source, edits);
	const auto displayed = source->transform(to_simple_transform(core::orientation::right_top));
	assert_equal(true, displayed->dimensions() == edited->dimensions(), "right-top edit dimensions match image view");
	for (auto y = 0u; y < edited->height(); ++y)
	{
		for (auto x = 0u; x < edited->width(); ++x)
		{
			assert_equal(displayed->get_pixel(x, y), edited->get_pixel(x, y), "right-top edit pixels match image view");
		}
	}
}

static void should_rotate_displayed_oriented_pixels_clockwise()
{
	const auto assert_orientation = [](const core::orientation orientation, const std::string_view message)
	{
		const auto source = std::make_shared<core::surface>();
		source->alloc(3, 2, core::texture_format::RGB);
		for (auto y = 0; y < 2; ++y)
		{
			for (auto x = 0; x < 3; ++x)
			{
				source->set_pixel(x, y, core::rgba(10 + x + y * 3, 0, 0));
			}
		}

		image_edits edits;
		edits.crop_bounds(
			rotate_command_crop_bounds(source->dimensions(), orientation, core::simple_transform::rot_90, true));
		const auto actual = apply_image_edits(source, edits);
		const auto displayed = source->transform(to_simple_transform(orientation));
		const auto expected = displayed->transform(core::simple_transform::rot_270);

		assert_equal(true, core::is_valid(actual), message, "actual rotated surface is valid");
		assert_equal(true, core::is_valid(expected), message, "expected rotated surface is valid");
		assert_equal(true, expected->dimensions() == actual->dimensions(), message, "dimensions");
		for (auto y = 0u; y < actual->height(); ++y)
		{
			for (auto x = 0u; x < actual->width(); ++x)
			{
				assert_equal(expected->get_pixel(x, y), actual->get_pixel(x, y), message, "pixels");
			}
		}
	};

	assert_orientation(core::orientation::right_top, "right_top");
	assert_orientation(core::orientation::left_bottom, "left_bottom");
}

static void should_build_rotated_edit_preview_surface()
{
	files ff;
	const auto loaded = ff.load(test_files_folder().combine_file("Test.jpg"), false);
	const auto source_dimensions = loaded.dimensions();
	const auto source = loaded.to_surface(core::scale_dimensions(source_dimensions, 192));

	image_edits edits;
	edits.crop_bounds(quadd(source_dimensions).transform(core::simple_transform::rot_90));
	const auto preview = edit_view::build_preview_surface(source, source_dimensions, edits);
	const auto expected_dimensions = core::scale_dimensions(sizei(source->height(), source->width()), 128);

	assert_equal(true, is_valid(preview), "rotated edit preview is valid");
	assert_equal(expected_dimensions.cx, static_cast<int>(preview->width()), "rotated edit preview width");
	assert_equal(expected_dimensions.cy, static_cast<int>(preview->height()), "rotated edit preview height");
}

static void should_build_straightened_edit_preview_without_black_corners()
{
	constexpr sizei loaded_dimensions(4000, 3000);
	const auto source = std::make_shared<core::surface>();
	source->alloc(192, 144, core::texture_format::RGB);
	source->clear(core::rgba(255, 255, 255));

	image_edits edits;
	const quadd image_bounds(loaded_dimensions);
	edits.crop_bounds(image_bounds.rotate(5.0, image_bounds.center_point()));
	const auto preview = edit_view::build_preview_surface(source, loaded_dimensions, edits);

	assert_equal(true, is_valid(preview), "straightened edit preview is valid");
	assert_equal(true, preview->width() <= 128 && preview->height() <= 128, "straightened edit preview is bounded");
	constexpr auto rgb_mask = 0x00ffffffu;
	constexpr auto white = 0x00ffffffu;
	assert_equal(white, preview->get_pixel(preview->width() / 2, preview->height() / 2) & rgb_mask,
	             "straightened edit preview center");
	assert_equal(white, preview->get_pixel(0, 0) & rgb_mask, "straightened edit preview top-left corner");
	assert_equal(white, preview->get_pixel(preview->width() - 1, 0) & rgb_mask,
	             "straightened edit preview top-right corner");
	assert_equal(white, preview->get_pixel(0, preview->height() - 1) & rgb_mask,
	             "straightened edit preview bottom-left corner");
	assert_equal(white, preview->get_pixel(preview->width() - 1, preview->height() - 1) & rgb_mask,
	             "straightened edit preview bottom-right corner");
}

static void should_not_treat_initial_orientation_as_an_edit()
{
	image_edits original_edits;
	original_edits.crop_bounds(edit_view_state::initial_crop(sizei(120, 80), core::orientation::right_top));
	auto current_edits = original_edits;

	assert_equal(false, current_edits != original_edits, "initial EXIF orientation is baseline");

	current_edits.crop_bounds(current_edits.crop_bounds().transform(core::simple_transform::rot_90));
	assert_equal(true, current_edits != original_edits, "user rotation changes baseline");
}

// Edit draws the stored orientation into the pixels it writes, so a saved edit of an EXIF-rotated
// JPEG kept Orientation=6 over pixels already upright and opened turned a second time.
static void should_save_oriented_edit_upright()
{
	assert_equal(false,
	             edit_view_state::save_metadata_edits(false, core::orientation::right_top).orientation.has_value(),
	             "no pixel edit leaves the orientation alone");
	assert_equal(false, edit_view_state::save_metadata_edits(true, core::orientation::top_left).orientation.has_value(),
	             "an upright photo needs no orientation change");

	files ff;
	const auto load_path = test_files_folder().combine_file("exif-rotated.jpg");
	const auto save_path = test_temps.next_path(".jpg");
	const auto loaded = ff.load(load_path, false);
	assert_equal(true, loaded.success, "exif-rotated.jpg loads");
	assert_equal(static_cast<int>(core::orientation::right_top), static_cast<int>(loaded.orientation()),
	             "the fixture is stored turned");

	// The same edit state the view builds: the stored orientation as the baseline crop, then a colour
	// change, which forces a re-encode rather than a lossless transform.
	image_edits edits;
	edits.crop_bounds(edit_view_state::initial_crop(loaded.dimensions(), loaded.orientation()));
	edits.brightness(0.25);

	const auto md_edits = edit_view_state::save_metadata_edits(true, loaded.orientation());
	assert_equal(true, ff.update(load_path, save_path, md_edits, edits, {}, false, {}).success(), "edit saves");

	const auto saved = ff.load(save_path, false);
	assert_equal(loaded.dimensions().cy, saved.dimensions().cx, "the saved pixels are upright");
	assert_equal(loaded.dimensions().cx, saved.dimensions().cy, "the saved pixels are upright");
	assert_equal(static_cast<int>(core::orientation::top_left), static_cast<int>(saved.orientation()),
	             "the saved photo is not turned again when shown");
	assert_equal(static_cast<int>(core::orientation::top_left),
	             static_cast<int>(extract_properties(save_path, metadata_type::EXIF)->orientation),
	             "the saved Exif orientation is upright");

	// PNG and WebP write the source Exif they are handed, so the reset has to reach it there too.
	const auto png_path = test_temps.next_path(".png");
	assert_equal(true, ff.update(load_path, png_path, md_edits, edits, {}, false, {}).success(), "edit saves as PNG");
	const auto png = ff.load(png_path, false);
	assert_equal(loaded.dimensions().cy, png.dimensions().cx, "the PNG pixels are upright");
	assert_equal(static_cast<int>(core::orientation::top_left), static_cast<int>(png.orientation()),
	             "a PNG saved from the edit is not turned again when shown");
}

// A JPEG libfiles' encoder writes of a picture dark on its left half and bright on its right, one channel
// or three, with the XMP packet given.
static std::vector<uint8_t> half_bright_jpeg(const sizei extent, const bool grey, const std::string_view xmp)
{
	const auto channels = grey ? 1 : 4;
	std::vector<uint8_t> pixels(static_cast<size_t>(extent.cx) * extent.cy * channels, 255);

	for (auto y = 0; y < extent.cy; ++y)
	{
		for (auto x = 0; x < extent.cx; ++x)
		{
			const auto level = static_cast<uint8_t>(x < extent.cx / 2 ? 40 : 215);
			auto* const pixel = pixels.data() + (static_cast<size_t>(y) * extent.cx + x) * channels;
			for (auto c = 0; c < std::min(channels, 3); ++c) pixel[c] = level;
		}
	}

	libfiles::embedded_metadata metadata;
	metadata.xmp = std::span(std::bit_cast<const uint8_t*>(xmp.data()), xmp.size());
	libfiles::jpeg_options options;
	options.quality = 95;
	options.grayscale = grey;
	libfiles::jpeg_encoder encoder;
	return encoder.encode(extent.cx, extent.cy, pixels.data(), static_cast<size_t>(extent.cx) * channels, 1, metadata,
	                      options);
}

// An edited Ultra HDR JPEG keeps its gain map: the same picture as the photo, at a quarter of its size, so
// wherever the photo is plainly dark or bright the gain map at the same place in its frame is too; and the
// photo's XMP gives the gain map's length.
static void assert_gain_map_registers(const core::file_path path, const std::string_view message)
{
	const auto saved = platform::blob_from_file(path);
	const auto gain_map = libfiles::find_jpeg_gain_map(saved);
	assert_equal(true, !gain_map.empty(), message, "the gain map is kept");
	if (gain_map.empty()) return;

	files ff;
	const auto photo = ff.image_to_surface(core::cspan(saved));
	const auto map = ff.image_to_surface(core::cspan(gain_map.data(), gain_map.size()));
	assert_equal(true, is_valid(photo) && is_valid(map), message, "the photo and its gain map decode");
	if (!is_valid(photo) || !is_valid(map)) return;

	const auto photo_aspect = static_cast<double>(photo->width()) / photo->height();
	const auto map_aspect = static_cast<double>(map->width()) / map->height();
	assert_equal(true, std::abs(photo_aspect - map_aspect) < 0.15, message, "the gain map has the photo's shape");
	assert_equal(true, map->width() * 3 < photo->width() && map->width() * 5 > photo->width(), message,
	             "the gain map keeps its scale");

	auto compared = 0;

	for (auto j = 0; j < 8; ++j)
	{
		for (auto i = 0; i < 8; ++i)
		{
			const auto u = (i + 0.5) / 8;
			const auto v = (j + 0.5) / 8;
			const auto photo_level = core::get_g(
				photo->get_pixel(static_cast<int>(u * photo->width()), static_cast<int>(v * photo->height())));
			const auto map_level = core::get_g(
				map->get_pixel(static_cast<int>(u * map->width()), static_cast<int>(v * map->height())));
			// Near the edge between the halves either may be mid grey.
			if ((photo_level > 80 && photo_level < 175) || (map_level > 80 && map_level < 175)) continue;
			assert_equal(photo_level > 128, map_level > 128, message, "the gain map lies over the photo");
			++compared;
		}
	}

	assert_equal(true, compared > 32, message, "most of the picture is compared");

	libfiles::reader r(std::span<const uint8_t>(saved.data(), saved.size()));
	libfiles::xmp::meta xmp;
	xmp.parse(libfiles::read_packet(r).packet);
	constexpr std::string_view container = "http://ns.google.com/photos/1.0/container/";
	constexpr std::string_view hdrgm = "http://ns.adobe.com/hdr-gain-map/1.0/";
	const auto container_prefix = libfiles::xmp::register_namespace(container, "Container");
	const auto item_prefix = libfiles::xmp::register_namespace("http://ns.google.com/photos/1.0/container/item/",
	                                                           "Item");
	libfiles::xmp::register_namespace(hdrgm, "hdrgm");
	std::string length;
	xmp.get_property(container, std::format("Directory[2]/{}Item/{}Length", container_prefix, item_prefix), &length);
	assert_equal(std::to_string(gain_map.size()), length, message, "the photo's XMP gives the gain map's length");
	assert_equal(true, xmp.exists(hdrgm, "Version"), message, "the photo's XMP says a gain map follows");
}

// An Ultra HDR JPEG's gain map says how much brighter than the photo each part of the scene is shown on an
// HDR display. A re-encode once dropped it and kept the XMP saying it was there.
static void should_keep_a_gain_map_through_an_edit()
{
	const auto primary_xmp = std::string(
		R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
		R"(<rdf:Description rdf:about="" xmlns:hdrgm="http://ns.adobe.com/hdr-gain-map/1.0/" )"
		R"(xmlns:Container="http://ns.google.com/photos/1.0/container/" )"
		R"(xmlns:Item="http://ns.google.com/photos/1.0/container/item/" hdrgm:Version="1.0"><Container:Directory><rdf:Seq>)"
		R"(<rdf:li rdf:parseType="Resource"><Container:Item Item:Semantic="Primary" Item:Mime="image/jpeg"/></rdf:li>)"
		R"(<rdf:li rdf:parseType="Resource"><Container:Item Item:Semantic="GainMap" Item:Mime="image/jpeg" Item:Length="1"/></rdf:li>)"
		R"(</rdf:Seq></Container:Directory></rdf:Description></rdf:RDF></x:xmpmeta>)");
	const auto gain_map_xmp = std::string(
		R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
		R"(<rdf:Description rdf:about="" xmlns:hdrgm="http://ns.adobe.com/hdr-gain-map/1.0/" hdrgm:Version="1.0" )"
		R"(hdrgm:GainMapMax="2.5" hdrgm:HDRCapacityMax="2.5"/></rdf:RDF></x:xmpmeta>)");

	// The photo's blocks allow a lossless quarter turn either way; the gain map's, a single channel 12 high,
	// one way only.
	constexpr sizei photo_extent(64, 48);
	const auto ultra_hdr = libfiles::attach_jpeg_gain_map(half_bright_jpeg(photo_extent, false, primary_xmp),
	                                                      half_bright_jpeg({16, 12}, true, gain_map_xmp));
	const auto source = test_temps.next_path(".jpg");
	assert_equal(true, platform::blob_save_to_file(core::cspan(ultra_hdr.data(), ultra_hdr.size()), source),
	             "the Ultra HDR JPEG is written");
	assert_gain_map_registers(source, "the source");

	const auto save = [&](const image_edits& edits, const std::string_view message)
	{
		files ff;
		const auto saved = test_temps.next_path(".jpg");
		assert_equal(true, ff.update(source, saved, {}, edits, {}, false, {}).success(), message, "the edit saves");
		assert_gain_map_registers(saved, message);
		return saved;
	};

	image_edits cropped;
	cropped.crop_bounds(quadd(rectd(0, 0, 48, 48)));
	cropped.brightness(0.2);
	files ff;
	const auto cropped_path = save(cropped, "cropped and brightened");
	assert_equal(48, ff.load(cropped_path, false).dimensions().cx, "the photo is cropped");

	// A turn one way moves the gain map's partial bottom blocks, which its lossless turn refuses; the other
	// way it is lossless.
	for (const auto turn : {core::simple_transform::rot_90, core::simple_transform::rot_270})
	{
		image_edits turned;
		turned.crop_bounds(quadd(photo_extent).transform(turn));
		const auto turned_path = save(turned, "turned");
		assert_equal(48, ff.load(turned_path, false).dimensions().cx, "the photo is turned");
	}

	image_edits straightened;
	const quadd bounds(photo_extent);
	straightened.crop_bounds(bounds.rotate(5.0, bounds.center_point()));
	straightened.scale(sizei(40, 40));
	save(straightened, "straightened and scaled");
}

// A picture a byte a channel cannot tell from flat: neighbouring 16-bit levels, which an edit keeps.
static void should_keep_sixteen_bits_through_an_edit()
{
	constexpr sizei extent(256, 4);
	const auto ramp = std::make_shared<core::surface>();
	ramp->alloc(extent, core::texture_format::RGBX16);

	for (auto y = 0; y < extent.cy; ++y)
	{
		auto* const row = std::bit_cast<uint16_t*>(ramp->pixels_line(y));

		for (auto x = 0; x < extent.cx; ++x)
		{
			row[x * 4 + 0] = static_cast<uint16_t>(30000 + x);
			row[x * 4 + 1] = static_cast<uint16_t>(30000 + x);
			row[x * 4 + 2] = static_cast<uint16_t>(30000 + x);
			row[x * 4 + 3] = 65535;
		}
	}

	const auto source = test_temps.next_path(".png");
	const auto png = save_png(ramp, {});
	assert_equal(true, is_valid(png) && platform::blob_save_to_file(png->data(), source), "the 16-bit PNG is written");

	files ff;
	const auto loaded = ff.load(source, false, decode_intent::edit);
	assert_equal(static_cast<int>(core::texture_format::RGBX16),
	             static_cast<int>(loaded.to_surface({}, false, {}, decode_intent::edit)->format()),
	             "an edit decodes it at sixteen bits");
	assert_equal(false, core::is_wide(loaded.to_surface()->format()), "the display, at a byte");

	image_edits edits;
	edits.crop_bounds(quadd(rectd(0, 0, 256, 2)));
	edits.brightness(0.1);
	const auto saved = test_temps.next_path(".png");
	assert_equal(true, ff.update(source, saved, {}, edits, {}, false, {}).success(), "the edit saves");

	const auto edited = ff.load(saved, false, decode_intent::edit).to_surface({}, false, {}, decode_intent::edit);
	assert_equal(true, is_valid(edited) && edited->format() == core::texture_format::RGBX16,
	             "the edit is saved at sixteen bits");
	if (!is_valid(edited)) return;

	std::set<uint16_t> levels;
	const auto* const row = std::bit_cast<const uint16_t*>(edited->pixels_line(0));
	for (auto x = 0; x < extent.cx; ++x) levels.insert(row[x * 4]);
	assert_equal(true, levels.size() > 200, "the levels a byte would merge stay apart");
	assert_equal(true, *levels.begin() > 30000, "and are brightened");
}

// A ramp of 16-bit levels 64 apart, which ten bits keep apart and a byte merges four at a time.
static core::surface_ptr deep_ramp(const sizei extent)
{
	auto ramp = std::make_shared<core::surface>();
	ramp->alloc(extent, core::texture_format::RGBX16);

	for (auto y = 0; y < extent.cy; ++y)
	{
		auto* const row = std::bit_cast<uint16_t*>(ramp->pixels_line(y));

		for (auto x = 0; x < extent.cx; ++x)
		{
			const auto level = static_cast<uint16_t>(16384 + x * 64);
			row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = level;
			row[x * 4 + 3] = 65535;
		}
	}

	return ramp;
}

static size_t levels_along_first_row(const core::const_surface_ptr& s)
{
	std::set<uint16_t> levels;
	const auto* const row = std::bit_cast<const uint16_t*>(s->pixels_line(0));
	for (uint32_t x = 0; x < s->width(); ++x) levels.insert(row[x * 4]);
	return levels.size();
}

static int stored_bit_depth(const core::file_path path)
{
	const auto file = platform::blob_from_file(path);
	libfiles::reader r(std::span<const uint8_t>(file.data(), file.size()));
	return libfiles::scan(r).picture.bit_depth;
}

// A 10- or 12-bit AVIF and a 16-bit JPEG XL were edited at a byte a channel and written back at that, over the
// photo. Each is edited at sixteen bits and saved at its own depth.
static void should_keep_a_photos_bits_through_an_edit_saved_over_it()
{
	constexpr sizei extent(256, 8);
	const auto ramp = deep_ramp(extent);

	for (const auto& [extension, bits] : {std::pair{".avif"sv, 10}, std::pair{".avif"sv, 12}, std::pair{".jxl"sv, 16}})
	{
		const auto message = std::format("{} at {} bits", extension, bits);
		// Lossless, so the levels counted are the ones written; an edit keeps a lossless file lossless.
		file_encode_params params;
		params.avif_lossless = true;
		params.avif_bit_depth = bits;
		params.jxl_lossless = true;
		const auto encoded = extension == ".avif" ? save_avif(ramp, {}, params) : save_jxl(ramp, {}, params);
		const auto path = test_temps.next_path(extension);
		assert_equal(true, is_valid(encoded) && platform::blob_save_to_file(encoded->data(), path), message,
		             "the photo is written");
		assert_equal(bits, stored_bit_depth(path), message, "at its depth");

		image_edits cropped;
		cropped.crop_bounds(quadd(rectd(0, 0, 256, 4)));
		files ff;
		assert_equal(true, ff.update(path, {}, cropped, {}, false, {}).success(), message, "the edit saves over it");
		assert_equal(bits, stored_bit_depth(path), message, "at the depth it had");

		const auto edited = ff.load(path, false, decode_intent::edit).to_surface({}, false, {}, decode_intent::edit);
		assert_equal(true, is_valid(edited) && edited->format() == core::texture_format::RGBX16, message,
		             "an edit decodes it at sixteen bits");
		if (!is_valid(edited)) continue;
		assert_equal(4, static_cast<int>(edited->height()), message, "cropped");
		assert_equal(true, levels_along_first_row(edited) > 200, message, "the levels a byte would merge stay apart");
	}
}

// A PQ picture: a ramp below SDR white, and a highlight far above it.
static core::surface_ptr pq_picture(const sizei extent)
{
	auto signal = std::make_shared<core::surface>();
	signal->alloc(extent, core::texture_format::RGBX16);

	for (auto y = 0; y < extent.cy; ++y)
	{
		auto* const row = std::bit_cast<uint16_t*>(signal->pixels_line(y));

		for (auto x = 0; x < extent.cx; ++x)
		{
			const auto level = static_cast<uint16_t>(x < 48 ? 20000 + x * 300 : 50000);
			row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = level;
			row[x * 4 + 3] = 65535;
		}
	}

	return signal;
}

// An edit of an HDR AVIF, JPEG XL or PNG replaced it with its SDR rendition. The signal is edited - its colour as its
// SDR rendition is shown changed - and saved as HDR; a JPEG holds the rendition with a gain map made for it, and a
// WebP the rendition alone.
static void should_keep_hdr_through_an_edit()
{
	constexpr sizei extent(64, 16);
	const auto signal = pq_picture(extent);
	libfiles::hdr_description hdr;
	hdr.max_cll = 1000;

	for (const auto extension : {".avif"sv, ".jxl"sv, ".png"sv})
	{
		const auto message = std::string(extension);
		file_encode_params params;
		params.hdr = hdr;
		params.avif_bit_depth = 12;
		params.jxl_lossless = true;
		const auto encoded = extension == ".avif"  ? save_avif(signal, {}, params)
		                     : extension == ".jxl" ? save_jxl(signal, {}, params)
		                                           : save_png(signal, {}, hdr);
		const auto path = test_temps.next_path(extension);
		assert_equal(true, is_valid(encoded) && platform::blob_save_to_file(encoded->data(), path), message,
		             "the HDR picture is written");

		files ff;
		const auto before = ff.load(path, false, decode_intent::edit);
		assert_equal(true, before.signal.hdr && before.signal.hdr->transfer == libfiles::hdr_transfer::pq, message,
		             "an edit loads its signal");
		const auto display = before.to_surface();
		assert_equal(true, is_valid(display) && !core::is_wide(display->format()), message,
		             "the display shows its SDR rendition");

		image_edits edits;
		edits.crop_bounds(quadd(rectd(0, 0, 64, 8)));
		edits.brightness(0.2);
		assert_equal(true, ff.update(path, {}, edits, {}, false, {}).success(), message, "the edit saves over it");

		const auto after = ff.load(path, false, decode_intent::edit);
		assert_equal(true, after.signal.hdr && after.signal.hdr->transfer == libfiles::hdr_transfer::pq, message,
		             "and keeps its HDR");
		const auto edited = after.to_surface({}, false, {}, decode_intent::edit);
		assert_equal(true, is_valid(edited) && edited->format() == core::texture_format::RGBX16, message,
		             "as its signal");
		if (!is_valid(edited)) continue;

		const auto* const row = std::bit_cast<const uint16_t*>(edited->pixels_line(0));
		assert_equal(8, static_cast<int>(edited->height()), message, "cropped");
		assert_equal(true, row[4 * 4] > 20000 + 4 * 300 + 500, message, "a dim part brightened");
		assert_equal(true, row[60 * 4] > 45000, message, "the highlight still far above SDR white");
	}

	const auto source = test_temps.next_path(".avif");
	file_encode_params params;
	params.hdr = hdr;
	const auto avif = save_avif(signal, {}, params);
	assert_equal(true, is_valid(avif) && platform::blob_save_to_file(avif->data(), source), "the HDR AVIF is written");

	files ff;
	const auto jpeg = test_temps.next_path(".jpg");
	assert_equal(true, ff.update(source, jpeg, {}, {}, {}, false, {}).success(), "saved as a JPEG");
	const auto jpeg_bytes = platform::blob_from_file(jpeg);
	const auto gain_map = libfiles::find_jpeg_gain_map({jpeg_bytes.data(), jpeg_bytes.size()});
	const auto metadata = gain_map.empty() ? std::nullopt : libfiles::read_jpeg_gain_map_metadata(gain_map);
	assert_equal(true, metadata.has_value(), "the JPEG holds a gain map");
	assert_equal(true, metadata && metadata->alternate_headroom > 1.0, "for the headroom the picture has");

	const auto webp = test_temps.next_path(".webp");
	assert_equal(true, ff.update(source, webp, {}, {}, {}, false, {}).success(), "saved as a WebP");
	const auto standard = ff.load(webp, false, decode_intent::edit);
	assert_equal(true, standard.success && !standard.signal.is_hdr(), "in standard range");
}

// An AVIF's gain map - ISO 21496-1's, as an iPhone's HEIC copy and Ultra HDR's AVIFs hold one - is edited with its
// photo and kept: in the AVIF saved over it, and after the photo in a JPEG.
static void should_keep_an_avif_gain_map_through_an_edit()
{
	constexpr sizei extent(64, 48);
	constexpr sizei map_extent(32, 24);
	const auto photo = std::make_shared<core::surface>();
	const auto map = std::make_shared<core::surface>();
	photo->alloc(extent, core::texture_format::RGB);
	map->alloc(map_extent, core::texture_format::RGB);

	for (auto y = 0; y < extent.cy; ++y)
	{
		auto* const line = photo->pixels_line(y);
		for (auto x = 0; x < extent.cx; ++x)
			line[x * 4] = line[x * 4 + 1] = line[x * 4 + 2] = static_cast<uint8_t>(x * 4);
	}

	for (auto y = 0; y < map_extent.cy; ++y)
	{
		auto* const line = map->pixels_line(y);
		for (auto x = 0; x < map_extent.cx; ++x)
			line[x * 4] = line[x * 4 + 1] = line[x * 4 + 2] = static_cast<uint8_t>(x * 8);
	}

	libfiles::gain_map_metadata metadata;
	metadata.alternate_headroom = 2;
	metadata.gain_max = {2, 2, 2};
	file_encode_params params;
	params.gain_map = gain_map_surface{map, metadata};
	const auto avif = save_avif(photo, {}, params);
	const auto path = test_temps.next_path(".avif");
	assert_equal(true, is_valid(avif) && platform::blob_save_to_file(avif->data(), path), "the AVIF is written");

	files ff;
	assert_equal(true, ff.load(path, false, decode_intent::edit).signal.gain_map, "an edit finds its gain map");

	image_edits cropped;
	cropped.crop_bounds(quadd(rectd(0, 0, 32, 48)));
	assert_equal(true, ff.update(path, {}, cropped, {}, false, {}).success(), "the crop saves over it");

	const auto file = platform::blob_from_file(path);
	mem_read_stream stream(file);
	const auto kept = load_heif_gain_map(stream);
	assert_equal(true, kept && is_valid(kept->pixels), "the gain map is kept");
	if (!kept || !is_valid(kept->pixels)) return;
	assert_equal(true, kept->pixels->dimensions() == sizei(16, 24), "cropped as the photo is");
	assert_equal(true, std::abs(kept->metadata.alternate_headroom - 2.0) < 0.01, "applying as it did");

	const auto jpeg = test_temps.next_path(".jpg");
	assert_equal(true, ff.update(path, jpeg, {}, {}, {}, false, {}).success(), "saved as a JPEG");
	const auto jpeg_bytes = platform::blob_from_file(jpeg);
	const auto carried = load_jpeg_gain_map({jpeg_bytes.data(), jpeg_bytes.size()});
	assert_equal(true, carried && is_valid(carried->pixels), "the JPEG holds it after the photo");
	assert_equal(true, carried && std::abs(carried->metadata.alternate_headroom - 2.0) < 0.01, "applying as it did");
}

// A photo the decode budget holds only at a byte a channel is edited at that, and the panel says so. An HDR one is
// edited as its SDR rendition, which is never written back over it - saved as a copy, and turned losslessly in place.
static void should_not_write_a_narrowed_hdr_photo_back_over_itself()
{
	constexpr sizei extent(64, 16);
	const auto restore_budget = render::max_decode_bytes;
	const core::scope_exit restore([restore_budget] { render::max_decode_bytes = restore_budget; });

	file_encode_params params;
	params.hdr = libfiles::hdr_description{};
	const auto hdr = save_avif(pq_picture(extent), {}, params);
	const auto hdr_path = test_temps.next_path(".avif");
	assert_equal(true, is_valid(hdr) && platform::blob_save_to_file(hdr->data(), hdr_path), "the HDR AVIF is written");

	file_encode_params lossless;
	lossless.avif_lossless = true;
	const auto deep = save_avif(deep_ramp(extent), {}, lossless);
	const auto deep_path = test_temps.next_path(".avif");
	assert_equal(true, is_valid(deep) && platform::blob_save_to_file(deep->data(), deep_path),
	             "the 10-bit AVIF is written");

	// Room for a byte a channel, and not for sixteen bits.
	render::max_decode_bytes = static_cast<int64_t>(extent.cx) * extent.cy * 6;
	files ff;

	const auto narrowed = ff.load(deep_path, false, decode_intent::edit);
	const auto narrowed_pixels = narrowed.to_surface({}, false, {}, decode_intent::edit);
	assert_equal(true, narrowed.signal.narrowed && !narrowed.signal.is_hdr(), "a deep photo is edited at a byte");
	assert_equal(true, is_valid(narrowed_pixels) && !core::is_wide(narrowed_pixels->format()), "its pixels a byte");
	assert_equal(std::string(tt.edit_too_large_for_depth.sv()), std::string(edit_photo_note(narrowed.signal)),
	             "which the panel says");

	const auto loaded = ff.load(hdr_path, false, decode_intent::edit);
	assert_equal(true, loaded.signal.tone_mapped && !loaded.signal.hdr, "an HDR photo is edited as its rendition");
	assert_equal(std::string(tt.edit_hdr_too_large.sv()), std::string(edit_photo_note(loaded.signal)),
	             "which the panel says");

	const auto before = platform::blob_from_file(hdr_path);
	image_edits cropped;
	cropped.crop_bounds(quadd(rectd(0, 0, 32, 16)));
	assert_equal(false, ff.update(hdr_path, {}, cropped, {}, false, {}).success(),
	             "its rendition written back over it is refused");
	const auto after = platform::blob_from_file(hdr_path);
	assert_equal(true, before.size() == after.size() && std::memcmp(before.data(), after.data(), before.size()) == 0,
	             "and the photo is left as it was");

	const auto copy = test_temps.next_path(".avif");
	assert_equal(true, ff.update(hdr_path, copy, {}, cropped, {}, false, {}).success(), "a copy is saved");
	assert_equal(false, ff.load(copy, false, decode_intent::edit).signal.is_hdr(), "in standard range");

	image_edits turned;
	turned.crop_bounds(quadd(extent).transform(core::simple_transform::rot_90));
	assert_equal(true, files::turns_in_place(hdr_path, turned, extent), "a quarter turn leaves its pixels as they are");
	assert_equal(true, ff.update(hdr_path, {}, turned, {}, false, {}).success(), "and is saved over it");
}

// A photo whose ICC profile describes colours other than sRGB's - a phone's Display P3 - is shown in sRGB's, never as
// planar YUV, and an edit adjusts what is shown: the save carries the change back into the profile's colours, and
// keeps the profile.
static void should_show_and_edit_a_wide_gamut_photo_as_srgb()
{
	constexpr sizei extent(32, 16);
	const auto profile = display_p3_profile();

	// Display P3's (187, 105, 62), which is sRGB's (200, 100, 50).
	const auto photo = std::make_shared<core::surface>();
	photo->alloc(extent, core::texture_format::RGB);

	for (auto y = 0; y < extent.cy; ++y)
	{
		auto* const line = photo->pixels_line(y);

		for (auto x = 0; x < extent.cx; ++x)
		{
			line[x * 4 + 0] = 62;
			line[x * 4 + 1] = 105;
			line[x * 4 + 2] = 187;
			line[x * 4 + 3] = 255;
		}
	}

	// The middle pixel's red, green and blue, where it is a byte a channel.
	const auto centre = [](const core::const_surface_ptr& s) -> std::array<int, 3>
	{
		if (!is_valid(s) || (s->format() != core::texture_format::RGB && s->format() != core::texture_format::ARGB))
			return {-1000, -1000, -1000};
		const auto* const p = s->pixels_line(static_cast<int>(s->height() / 2)) + (s->width() / 2) * 4;
		return {p[2], p[1], p[0]};
	};
	const auto near = [](const std::array<int, 3>& a, const std::array<int, 3>& b, const int levels)
	{
		return std::abs(a[0] - b[0]) <= levels && std::abs(a[1] - b[1]) <= levels && std::abs(a[2] - b[2]) <= levels;
	};

	libfiles::metadata_blocks metadata;
	metadata.icc = profile;
	file_encode_params params;
	params.webp_lossless = true;

	for (const auto extension : {".png"sv, ".webp"sv, ".jpg"sv})
	{
		const auto message = std::string(extension);
		const auto jpeg = extension == ".jpg";
		const auto levels = jpeg ? 5 : 2;
		const auto encoded = extension == ".png"    ? save_png(photo, metadata)
		                     : extension == ".webp" ? save_webp(photo, metadata, params)
		                                            : save_jpeg(photo, metadata, params);
		const auto path = test_temps.next_path(extension);
		assert_equal(true, is_valid(encoded) && platform::blob_save_to_file(encoded->data(), path), message,
		             "the photo is written with its profile");

		files ff;
		const auto shown = ff.load(path, false).to_surface({}, true);
		assert_equal(true, near(centre(shown), {200, 100, 50}, levels), message,
		             "shown in sRGB's colours, never planar");

		std::string said;
		for (const auto& block : ff_scan_file(ff, path).to_info().metadata)
			if (block.standard == metadata_standard::icc)
				for (const auto& row : block.values)
					if (row.key == "Shown") said = row.value;
		assert_equal("mapped to sRGB"s, said, message, "which the info pane says beside the profile");

		const auto loaded = ff.load(path, false, decode_intent::edit);
		assert_equal(true, loaded.signal.profile == profile, message, "an edit finds its profile");
		const auto pixels = loaded.to_surface({}, false, {}, decode_intent::edit);
		assert_equal(true, near(centre(pixels), {187, 105, 62}, levels), message, "an edit has the file's own colours");

		// Saturation tells the spaces apart: adjusted as the file's own colours, they would show more saturated.
		image_edits edits;
		edits.brightness(0.2);
		edits.saturation(0.5);
		const auto expected = centre(apply_image_edits(profile_rendition(pixels, profile, true), edits));
		assert_equal(true, ff.update(path, {}, edits, {}, false, {}).success(), message, "the edit saves over it");

		const auto saved = platform::blob_from_file(path);
		libfiles::reader saved_reader(std::span<const uint8_t>(saved.data(), saved.size()));
		assert_equal(true, libfiles::scan(saved_reader).metadata.icc == profile, message, "the profile is kept");
		const auto after = centre(ff.load(path, false).to_surface());
		assert_equal(true, expected[0] > 210 && near(after, expected, levels), message, "shown as the edit showed it");
	}
}

// A raw file is developed at sixteen bits a channel for an edit, and a byte a channel for the display.
static void should_develop_a_raw_at_sixteen_bits_for_an_edit()
{
	const auto path = test_files_folder().combine_file("Gherkin.CR2");
	const auto display = load_raw(path, false);
	const auto edit = load_raw(path, false, true);
	assert_equal(true, display.success && is_valid(display.s) && edit.success && is_valid(edit.s), "both develop");
	if (!is_valid(display.s) || !is_valid(edit.s)) return;

	assert_equal(static_cast<int>(core::texture_format::RGB), static_cast<int>(display.s->format()), "a byte");
	assert_equal(static_cast<int>(core::texture_format::RGBX16), static_cast<int>(edit.s->format()), "sixteen bits");
	assert_equal(true, display.s->dimensions() == edit.s->dimensions(), "the same picture");
	assert_equal(static_cast<int>(display.s->orientation()), static_cast<int>(edit.s->orientation()), "turned alike");
}

static void should_update_rating(const std::string_view name)
{
	files ff;

	const auto ext = name.substr(core::find_ext(name));
	const auto save_path = test_temps.next_path(ext);
	const auto load_path = test_files_folder().combine_file(name);

	metadata_edits edits1;
	edits1.rating = 3;

	auto set_rating = ff.update(load_path, save_path, edits1, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));

	{
		const auto actual_scanned = ff_scan_after_update(ff, set_rating, save_path, detect_xmp_sidecar(save_path));
		const auto ps = actual_scanned.to_props();
		assert_equal(3, ps->rating, "rating");
	}

	metadata_edits edits2;
	edits2.remove_rating = true;
	auto clear_rating = ff.update(save_path, edits2, {}, {}, false, {}, ff_inspect_rescan(save_path));

	{
		const auto actual_scanned = ff_scan_after_update(ff, clear_rating, save_path, detect_xmp_sidecar(save_path));
		const auto ps = actual_scanned.to_props();
		assert_equal(0, ps->rating, "rating");
	}
}

static void should_update_label(const std::string_view name)
{
	files ff;

	const auto ext = name.substr(core::find_ext(name));
	const auto save_path = test_temps.next_path(ext);
	const auto load_path = test_files_folder().combine_file(name);

	metadata_edits set_label;
	set_label.label = label_approved_text;
	auto label_written = ff.update(load_path, save_path, set_label, {}, {}, false, {}, {},
	                               ff_inspect_rescan(save_path));

	{
		const auto actual_scanned = ff_scan_after_update(ff, label_written, save_path, detect_xmp_sidecar(save_path));
		assert_equal(label_approved_text, actual_scanned.to_props()->label, "label");
	}

	metadata_edits clear_label;
	clear_label.label = std::string{};
	auto label_cleared = ff.update(save_path, clear_label, {}, {}, false, {}, ff_inspect_rescan(save_path));

	{
		const auto actual_scanned = ff_scan_after_update(ff, label_cleared, save_path, detect_xmp_sidecar(save_path));
		assert_equal(std::string_view{}, actual_scanned.to_props()->label, "cleared label");
	}
}

// Every format diffractor embeds metadata in is libfiles' to write. The packet's own toolkit identifier
// says which wrote it, and a second edit that fits the first packet must leave the file the same size.
static void should_write_packets_with_libfiles(const std::string_view name)
{
	files ff;

	const auto ext = name.substr(core::find_ext(name));
	const auto save_path = test_temps.next_path(ext);
	const auto load_path = test_files_folder().combine_file(name);

	metadata_edits first;
	first.rating = 2;
	auto written = ff.update(load_path, save_path, first, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));
	assert_equal(true, written.success(), std::format("{} written ({})", name, written.format_error()));

	const auto scanned = ff_scan_after_update(ff, written, save_path);
	const std::string_view packet(std::bit_cast<const char*>(scanned.metadata.xmp.data()), scanned.metadata.xmp.size());
	assert_equal(true, packet.find("libfiles XMP") != std::string_view::npos,
	             std::format("{} packet from libfiles", name));
	assert_equal(2, scanned.to_props()->rating, "rating");

	const auto size_before = platform::file_attributes(save_path).size;

	metadata_edits second;
	second.rating = 4;
	auto rewritten = ff.update(save_path, second, {}, {}, false, {}, ff_inspect_rescan(save_path));
	assert_equal(true, rewritten.success(), std::format("{} rewritten ({})", name, rewritten.format_error()));

	const auto rescanned = ff_scan_after_update(ff, rewritten, save_path);
	assert_equal(4, rescanned.to_props()->rating, "second rating");
	assert_equal(size_before, platform::file_attributes(save_path).size, "a packet that fits keeps the file size");
}

// A camera's JPEG given an IPTC title and keywords and no packet: what it shows that only its IPTC holds.
static core::file_path iptc_titled_jpeg()
{
	const auto source = test_temps.next_path(".jpg");
	const auto fixture = platform::blob_from_file(test_files_folder().combine_file("IMG_9340.JPG"));
	const std::vector<uint8_t> bytes(fixture.begin(), fixture.end());
	libfiles::reader r(bytes);

	libfiles::metadata_update iptc_only;
	iptc_only.legacy.title = "Converted title";
	iptc_only.legacy.keywords = std::vector<std::string>{"alpha", "beta"};
	const auto plan = libfiles::plan_update(r, iptc_only);
	assert_equal(true, plan.ok(), "source planned");

	libfiles::memory_stream out;
	libfiles::write_updated(r, plan, out);
	const auto& written = out.bytes();
	assert_equal(true, platform::blob_save_to_file({written.data(), written.size()}, source), "source stored");
	return source;
}

// A converted file keeps what its source shows, even where the source keeps it somewhere the new format
// has no place for: IPTC, in a JPEG saved as PNG. The toolkit carried it by importing the source's IPTC
// into the packet; the packet now takes it from what the item shows.
static void should_carry_shown_metadata_into_a_converted_file()
{
	const auto source = iptc_titled_jpeg();

	files ff;
	const auto before = ff_scan_file(ff, source);
	assert_equal(true, before.metadata.xmp.empty(), "source holds no packet");
	assert_equal("Converted title"_c, before.to_props()->title, "source shows its IPTC title");

	const auto destination = test_temps.next_path(".png");
	platform::copy_file(test_files_folder().combine_file("engine.png"), destination, false, false);

	metadata_edits edits;
	edits.rating = 3;
	const auto result = metadata_xmp::update(destination, source, edits, {}, {});
	assert_equal(true, result.success, "converted file written");

	const auto after = ff_scan_file(ff, destination);
	const auto props = after.to_props();
	assert_equal("Converted title"_c, props->title, "title carried into the packet");
	const auto tags = props->tags.sv();
	assert_equal(true, core::contains(tags, "alpha") && core::contains(tags, "beta"), "tags carried");
	assert_equal(3, props->rating, "the edit applied");
}

// libfiles writes no packet into a JPEG XL, so a conversion into one puts what the source shows - here
// an IPTC title and keywords, which a JPEG XL has nowhere else to keep - and the edit into the packet it
// is encoded with, beside the source's EXIF.
static void should_carry_shown_metadata_into_a_jxl()
{
	const auto source = iptc_titled_jpeg();

	files ff;
	const auto before = ff_scan_file(ff, source);
	assert_equal(true, before.metadata.xmp.empty(), "source holds no packet");

	const auto destination = test_temps.next_path(".jxl");
	metadata_edits edits;
	edits.rating = 3;
	const auto result = ff.update(source, destination, edits, {}, {}, false, {});
	assert_equal(true, result.success(), std::format("converted ({})", result.format_error()));
	assert_equal(false, platform::exists(destination.extension(".xmp")), "no sidecar beside a JPEG XL");

	const auto after = ff_scan_file(ff, destination);
	assert_equal(false, after.metadata.xmp.empty(), "the packet is boxed");
	assert_equal(false, after.metadata.exif.empty(), "the EXIF is boxed");

	const auto props = after.to_props();
	const auto shown = before.to_props();
	assert_equal("Converted title"_c, props->title, "title carried into the packet");
	const auto tags = props->tags.sv();
	assert_equal(true, core::contains(tags, "alpha") && core::contains(tags, "beta"), "tags carried");
	assert_equal(3, props->rating, "the edit applied");
	assert_equal(shown->camera_model, props->camera_model, "camera carried in the EXIF");
	assert_equal(shown->dates.original(), props->dates.original(), "capture date carried");
}

// A JPEG from its first scan to its end: its picture, whatever the segments before it hold.
static std::vector<uint8_t> jpeg_scans(const core::cspan jpeg)
{
	size_t pos = 2;

	while (pos + 4 <= jpeg.size && jpeg.data[pos] == 0xFF)
	{
		if (jpeg.data[pos + 1] == 0xDA) return {jpeg.data + pos, jpeg.data + jpeg.size};
		pos += 2 + (size_t{jpeg.data[pos + 2]} << 8 | jpeg.data[pos + 3]);
	}

	return {};
}

// A JPEG converted to a lossless JPEG XL is repacked rather than encoded from its pixels: smaller, and keeping
// the JPEG it gives back, its picture byte for byte. What the JPEG shows from its IPTC alone - kept by the JPEG
// XL only in the box it gives the JPEG back from - goes into the packet as any conversion's does. Converted to
// a JPEG again it is that JPEG, whatever quality the conversion names; converted lossily it is encoded afresh.
static void should_repack_a_jpeg_as_a_lossless_jxl_that_gives_it_back()
{
	const auto source = iptc_titled_jpeg();
	const auto source_bytes = platform::blob_from_file(source);

	files ff;
	const auto shown = ff_scan_file(ff, source).to_props();

	const auto repacked = test_temps.next_path(".jxl");
	metadata_edits edits;
	edits.rating = 3;
	file_encode_params lossless;
	lossless.jxl_lossless = true;
	lossless.reencode = true;
	const auto result = ff.update(source, repacked, edits, {}, lossless, false, {});
	assert_equal(true, result.success(), std::format("repacked ({})", result.format_error()));

	const auto repacked_bytes = platform::blob_from_file(repacked);
	const auto given_back = libfiles::reconstruct_jpeg({repacked_bytes.data(), repacked_bytes.size()});
	assert_equal(false, given_back.empty(), "the JPEG XL gives a JPEG back");
	assert_equal(true, jpeg_scans(source_bytes) == jpeg_scans({given_back.data(), given_back.size()}),
	             "whose picture is the JPEG's, byte for byte");
	assert_equal(true, repacked_bytes.size() < source_bytes.size(), "and the JPEG XL is the smaller");

	const auto props = ff_scan_file(ff, repacked).to_props();
	assert_equal("Converted title"_c, props->title, "the IPTC title carried into the packet");
	const auto tags = props->tags.sv();
	assert_equal(true, core::contains(tags, "alpha") && core::contains(tags, "beta"), "and the keywords");
	assert_equal(3, props->rating, "the edit applied");
	assert_equal(shown->camera_model, props->camera_model, "the camera in the EXIF");

	const auto back = test_temps.next_path(".jpg");
	file_encode_params converted;
	converted.reencode = true;
	converted.jpeg_save_quality = 40;
	const auto returned = ff.update(repacked, back, {}, {}, converted, false, {});
	assert_equal(true, returned.success(), std::format("converted back ({})", returned.format_error()));
	assert_equal(true, jpeg_scans(source_bytes) == jpeg_scans(platform::blob_from_file(back)),
	             "converted back, the picture is the JPEG's");
	const auto back_props = ff_scan_file(ff, back).to_props();
	assert_equal("Converted title"_c, back_props->title, "with what the JPEG XL showed");
	assert_equal(3, back_props->rating, "rating and all");

	const auto lossy = test_temps.next_path(".jxl");
	file_encode_params lossy_params;
	lossy_params.reencode = true;
	assert_equal(true, ff.update(source, lossy, {}, {}, lossy_params, false, {}).success(), "converted lossily");
	const auto lossy_bytes = platform::blob_from_file(lossy);
	assert_equal(true, libfiles::reconstruct_jpeg({lossy_bytes.data(), lossy_bytes.size()}).empty(),
	             "a lossy JPEG XL is encoded from the pixels");
}

// A JPEG of a gradient, as an encoder writes one, holding the EXIF given.
static std::vector<uint8_t> gradient_jpeg(const uint32_t width, const uint32_t height,
                                          const std::span<const uint8_t> exif = {})
{
	std::vector<uint8_t> pixels(size_t{width} * height * 4);

	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			auto* const p = pixels.data() + (size_t{y} * width + x) * 4;
			p[0] = static_cast<uint8_t>(x * 4);
			p[1] = static_cast<uint8_t>(y * 5);
			p[2] = static_cast<uint8_t>(255 - x * 3);
		}
	}

	libfiles::embedded_metadata metadata;
	metadata.exif = exif;
	libfiles::jpeg_encoder encoder;
	return encoder.encode(width, height, pixels.data(), size_t{width} * 4, 1, metadata, {});
}

// A JPEG XL repacked from a JPEG turns as that JPEG would, and gives back the JPEG turned with it: by the orientation
// its EXIF holds, written there and in the codestream; else turned without loss and repacked; else - its sides not
// whole blocks - encoded again against its own tables and repacked.
static void should_turn_a_jxl_repacked_from_a_jpeg_as_the_jpeg_would_turn()
{
	const auto repacked = [](const std::vector<uint8_t>& jpeg)
	{
		const auto path = test_temps.next_path(".jxl");
		const auto jxl = libfiles::transcode_jpeg_to_jxl(jpeg).data;
		assert_equal(true, !jxl.empty() && platform::blob_save_to_file({jxl.data(), jxl.size()}, path), "repacked");
		return path;
	};

	const auto given_back = [](const core::file_path path)
	{
		const auto file = platform::blob_from_file(path);
		return libfiles::reconstruct_jpeg({file.data(), file.size()});
	};

	const auto scans = [](const std::vector<uint8_t>& jpeg)
	{
		return jpeg_scans({jpeg.data(), jpeg.size()});
	};

	files ff;

	libfiles::exif_fields upright;
	upright.orientation = 1;
	const auto upright_exif = libfiles::make_exif(upright);
	const auto tagged = gradient_jpeg(64, 48, upright_exif);
	const auto tagged_path = repacked(tagged);
	const auto by_exif = ff_turn(ff, tagged_path, simple_transform::rot_90);
	assert_equal(true, by_exif.success(), std::format("turned by its EXIF ({})", by_exif.format_error()));
	const auto tagged_back = given_back(tagged_path);
	assert_equal(true, !tagged_back.empty() && scans(tagged_back) == scans(tagged), "giving back the JPEG it was");
	libfiles::reader tagged_reader(tagged_back);
	assert_equal(6, static_cast<int>(libfiles::scan(tagged_reader).orientation.value_or(1)), "turned by its EXIF");

	const auto plain = gradient_jpeg(64, 48);
	const auto plain_path = repacked(plain);
	const auto losslessly = ff_turn(ff, plain_path, simple_transform::rot_90);
	assert_equal(true, losslessly.success(), std::format("turned without loss ({})", losslessly.format_error()));
	libfiles::jpeg_transformer transformer;
	libfiles::jpeg_encoder encoder;
	const auto turned = transformer.turn(plain, encoder, 6, std::numeric_limits<uint64_t>::max()).data;
	const auto plain_back = given_back(plain_path);
	assert_equal(true, !turned.empty() && !plain_back.empty() && scans(plain_back) == scans(turned),
	             "giving back the JPEG turned without loss");

	const auto ragged_path = repacked(gradient_jpeg(60, 44));
	const auto before = ff_shown_picture(ff, ragged_path);
	const auto encoded = ff_turn(ff, ragged_path, simple_transform::rot_90);
	assert_equal(true, encoded.success(), std::format("turned and encoded again ({})", encoded.format_error()));
	assert_equal(false, given_back(ragged_path).empty(), "still giving a JPEG back");
	const auto expected = before->transform(simple_transform::rot_270);
	const auto after = ff_shown_picture(ff, ragged_path);
	assert_equal(true, expected->dimensions() == after->dimensions(), "shows turned");
	const auto mean = mean_channel_difference(expected, after);
	assert_equal(true, mean < 4.0, std::format("its picture turned clockwise ({})", mean));
}

// A JPEG XL takes a metadata edit where it keeps its packet. The fixture is libjxl's, its EXIF and packet
// Brotli-compressed between the two parts of its codestream: read with Diffractor's own Brotli, the first
// edit writes them plain where they stood, and the next, which fits, is patched. The picture never changes.
static void should_rate_and_tag_a_jxl()
{
	const auto path = test_temps.next_path(".jxl");
	platform::copy_file(test_files_folder().combine_file("zoltan-tasi.jxl"), path, false, false);

	files ff;
	const auto before = ff_scan_file(ff, path);
	const auto pixels_before = ff.load(path, false);
	assert_equal(true, before.success && pixels_before.success, "the fixture reads");

	metadata_edits first;
	first.rating = 4;
	first.add_tags = tag_set("harbour");
	const auto rated = ff.update(path, first, {}, {}, false, {});
	assert_equal(true, rated.success(), std::format("rated ({})", rated.format_error()));
	assert_equal(false, platform::exists(path.extension(".xmp")), "no sidecar beside a JPEG XL");

	const auto after = ff_scan_file(ff, path);
	const auto props = after.to_props();
	assert_equal(4, props->rating, "the rating reads back");
	assert_equal(true, core::contains(props->tags.sv(), "harbour"), "the tag reads back");
	assert_equal(before.width, after.width, "the size is the codestream's still");
	assert_equal(before.to_props()->camera_model, props->camera_model, "the EXIF is kept");

	metadata_edits second;
	second.rating = 2;
	const auto again = ff.update(path, second, {}, {}, false, {});
	assert_equal(true, again.success(), std::format("rated again ({})", again.format_error()));
	assert_equal(false, again.staged, "an edit that fits is patched where it lies");
	assert_equal(2, ff_scan_file(ff, path).to_props()->rating, "the second rating reads back");

	const auto pixels_after = ff.load(path, false);
	assert_equal(true, pixels_after.success, "the picture reads");
	assert_equal(true,
	             pixels_before.to_surface()->pixel_difference(pixels_after.to_surface()) ==
	                 core::pixel_difference_result::equal,
	             "the picture is unchanged");
}

// A HEIC or an AVIF takes a metadata edit in its own items, as libheif adds them, with no sidecar: the first edit
// gives the file a packet and writes the place to the EXIF it holds; the next, which fits, is patched where the file
// lies. The picture never changes.
static void should_rate_and_tag_a_heif(const std::string_view name)
{
	const auto path = test_temps.next_path(name.substr(core::find_ext(name)));
	platform::copy_file(test_files_folder().combine_file(name), path, false, false);

	files ff;
	const auto before = ff_scan_file(ff, path);
	const auto pixels_before = ff.load(path, false);
	assert_equal(true, before.success && pixels_before.success, std::format("{} reads", name));

	metadata_edits first;
	first.rating = 4;
	first.add_tags = tag_set("harbour");
	first.location_coordinate = gps_coordinate(50.0875, 14.4214);
	const auto rated = ff.update(path, first, {}, {}, false, {});
	assert_equal(true, rated.success(), std::format("{} rated ({})", name, rated.format_error()));
	assert_equal(false, platform::exists(path.extension(".xmp")), "no sidecar beside a HEIF");

	const auto after = ff_scan_file(ff, path);
	const auto props = after.to_props();
	assert_equal(4, props->rating, "the rating reads back");
	assert_equal(true, core::contains(props->tags.sv(), "harbour"), "the tag reads back");
	assert_equal({50.0875, 14.4214}, props->coordinate, "the place reads back");
	assert_equal(before.width, after.width, "the size is the image's still");
	assert_equal(before.to_props()->camera_model, props->camera_model, "the EXIF is kept");

	if (!before.metadata.exif.empty())
	{
		assert_equal({50.0875, 14.4214}, extract_properties(path, metadata_type::EXIF)->coordinate,
		             "the EXIF holds the place too");
	}

	metadata_edits second;
	second.rating = 2;
	const auto again = ff.update(path, second, {}, {}, false, {});
	assert_equal(true, again.success(), std::format("{} rated again ({})", name, again.format_error()));
	assert_equal(false, again.staged, "an edit that fits is patched where it lies");
	assert_equal(2, ff_scan_file(ff, path).to_props()->rating, "the second rating reads back");

	const auto pixels_after = ff.load(path, false);
	assert_equal(true, pixels_after.success, "the picture reads");
	assert_equal(true,
	             pixels_before.to_surface()->pixel_difference(pixels_after.to_surface()) ==
	                 core::pixel_difference_result::equal,
	             "the picture is unchanged");
}

// A JPEG XL with no metadata is a bare codestream, as Diffractor writes one; its first packet puts it in a
// container, the codestream inside it unchanged.
static void should_rate_a_bare_jxl()
{
	files ff;
	const auto source = ff.load(test_files_folder().combine_file("Test.jpg"), false);
	const auto image = save_jxl(source.to_surface(), {}, {});
	assert_equal(true, image != nullptr, "encoded");

	const auto path = test_temps.next_path(".jxl");
	assert_equal(true, platform::blob_save_to_file(image->data(), path), "stored");
	const auto bare = platform::blob_from_file(path);
	assert_equal(true, bare.size() > 2 && bare[0] == 0xFF && bare[1] == 0x0A, "written bare");

	metadata_edits edits;
	edits.rating = 5;
	const auto result = ff.update(path, edits, {}, {}, false, {});
	assert_equal(true, result.success(), std::format("rated ({})", result.format_error()));

	const auto boxed = platform::blob_from_file(path);
	assert_equal(true, boxed.size() > 12 && boxed[3] == 0x0C && boxed[4] == 'J', "now a container");
	assert_equal(true,
	             boxed.size() > bare.size() &&
	                 std::ranges::equal(std::span(bare.data(), bare.size()),
	                                    std::span(boxed.data() + (boxed.size() - bare.size()), bare.size())),
	             "the codestream is kept whole");
	assert_equal(5, ff_scan_file(ff, path).to_props()->rating, "the rating reads back");
}

// JPX and WBMP have nowhere Diffractor writes metadata, so an edit goes to a sidecar, as a raw file's
// does, and the image is left exactly as it was.
static void should_write_a_sidecar_for_an_image_without_a_writer(const std::string_view ext)
{
	const auto path = test_temps.next_path(ext);

	// A WBMP of 8 by 2 pixels, or a JPEG 2000 signature box and file type box.
	const std::vector<uint8_t> image = ext == ".wbmp"
	                                       ? std::vector<uint8_t>{0, 0, 8, 2, 0xAA, 0x55}
	                                       : std::vector<uint8_t>{0,    0,    0,    0x0C, 'j', 'P', ' ', ' ',
	                                                              0x0D, 0x0A, 0x87, 0x0A, 0,   0,   0,   0x14,
	                                                              'f',  't',  'y',  'p',  'j', 'p', 'x', ' ',
	                                                              0,    0,    0,    0,    'j', 'p', 'x', ' '};
	assert_equal(true, platform::blob_save_to_file({image.data(), image.size()}, path), "image stored");

	files ff;
	metadata_edits edits;
	edits.rating = 3;
	const auto result = ff.update(path, edits, {}, {}, false, {});
	assert_equal(true, result.success(), std::format("edit saved ({})", result.format_error()));

	const auto image_after = platform::blob_from_file(path);
	assert_equal(true, std::ranges::equal(image, image_after), "image unchanged");

	const auto sidecar = platform::blob_from_file(path.extension(".xmp"));
	assert_equal(false, sidecar.empty(), "sidecar written");

	libfiles::xmp::meta packet;
	packet.parse(std::string_view(std::bit_cast<const char*>(sidecar.data()), sidecar.size()));
	std::int32_t rating = 0;
	assert_equal(true, packet.get_property_int(libfiles::xmp::ns::xmp, "Rating", &rating) && rating == 3,
	             "rating in the sidecar");
}

// An ID3 tag before a FLAC stream does not make a file MPEG audio, whatever its name says, and libfiles
// cannot update FLAC: an edit is refused as one the format cannot store, and the file is left as it was.
// Before, the gate read the first bytes as an MP3's, and the edit failed in libfiles' plan instead.
static void should_refuse_an_edit_to_flac_behind_an_id3_tag()
{
	// An ID3v2.3 tag of padding alone, then a FLAC stream's signature and STREAMINFO: 44.1 kHz stereo,
	// 16 bits, one second.
	std::string file("ID3\x03\x00\x00\x00\x00\x00\x0A", 10);
	file.append(10, '\0');
	file += "fLaC";
	file.append("\x80\x00\x00\x22", 4);
	file.append("\x10\x00\x10\x00\x00\x00\x00\x00\x00\x00\x0A\xC4\x42\xF0\x00\x00\xAC\x44", 18);
	file.append(16, '\0');

	const auto path = test_temps.next_path(".mp3");
	write_test_file(path, file);

	files ff;
	metadata_edits edits;
	edits.rating = 3;
	const auto result = ff.update(path, edits, {}, {}, false, {});
	assert_equal(false, result.success(), "the edit is refused");
	assert_equal(true, core::contains(result.format_error(), "this file format cannot store the metadata"),
	             std::format("refused as a format that cannot store metadata ({})", result.format_error()));
	assert_equal(file, read_test_file(path), "the file is left as it was");
}

static void should_roundtrip_xmp_disc_metadata()
{
	const auto source = test_files_folder().combine_file("Byzantium.avi");
	const auto path = test_temps.next_path(source.extension());
	platform::copy_file(source, path, false, false);

	files ff;
	metadata_edits edits;
	edits.disk_num = core::xy8{2, 3};
	auto result = ff.update(path, edits, {}, {}, false, {}, ff_inspect_rescan(path));
	assert_equal(true, result.success(), std::format("disc metadata saved ({})", result.format_error()));

	const auto scanned = ff_scan_after_update(ff, result, path, detect_xmp_sidecar(path));
	const auto props = scanned.to_props();
	assert_equal(2, static_cast<int>(props->disk.x), "disc number round trips from XMP");
	assert_equal(3, static_cast<int>(props->disk.y), "disc total round trips from XMP");
}

// Issue #252 - a rotated video reported its rotation until a label was applied, after which the
// tile flipped from portrait to landscape. The recorded orientation has to survive the write, so
// this scans the rotated fixture, writes a label, and reads the orientation back.
static void should_keep_video_rotation_through_a_metadata_write()
{
	files ff;

	const auto save_path = test_temps.next_path(".mp4");
	const auto load_path = test_files_folder().combine("excluded1").combine_file("rotated90.mp4");

	const auto before = ff_scan_file(ff, load_path);
	assert_equal(true, before.success, "the rotated fixture scans");
	assert_equal(560u, before.width, "coded width");
	assert_equal(320u, before.height, "coded height");
	assert_equal(core::orientation::right_top, before.to_props()->orientation,
	             "the fixture is a video carrying a 90 degree display matrix");

	metadata_edits set_label;
	set_label.label = label_approved_text;
	auto written = ff.update(load_path, save_path, set_label, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));

	const auto scanned = ff_scan_after_update(ff, written, save_path, detect_xmp_sidecar(save_path));
	const auto ps = scanned.to_props();

	assert_equal(label_approved_text, ps->label, "label written to the rotated video");
	assert_equal(core::orientation::right_top, ps->orientation, "rotation survives a metadata write");
}

// Issue #134 - ratings and labels could not be applied to files whose name contains emoji.
// Emoji are non-BMP: surrogate pairs in the UTF-16 filesystem API and 4-byte sequences in the
// UTF-8 path the model carries. The in-place update path is the risky one - it derives a
// temporary name from the original, writes it, then replaces the original - so the name has to
// survive creation, a rescan, the replacement, and a second rescan without being mangled.
static void should_update_rating_and_label_for_emoji_filename()
{
	files ff;

	constexpr std::string_view emoji_name = "test-\U0001F389\U0001F4A3.jpg"; // test-🎉💣.jpg

	const auto folder = test_temps.next_folder("emoji-names");
	const auto save_path = folder.combine_file(emoji_name);
	const auto load_path = test_files_folder().combine_file("Test.jpg");

	metadata_edits create_edits;
	create_edits.rating = 3;
	auto created = ff.update(load_path, save_path, create_edits, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));

	assert_equal(true, platform::exists(save_path), "emoji-named file created");
	assert_equal(emoji_name, save_path.name().sv(), "emoji name is not mangled on write");

	{
		const auto scanned = ff_scan_after_update(ff, created, save_path, detect_xmp_sidecar(save_path));
		assert_equal(3, scanned.to_props()->rating, "rating written to emoji-named file");
	}

	// In-place: this is the temp-file-then-replace path, and the only one the report exercises.
	metadata_edits label_edits;
	label_edits.label = label_approved_text;
	auto labelled = ff.update(save_path, label_edits, {}, {}, false, {}, ff_inspect_rescan(save_path));

	{
		const auto scanned = ff_scan_after_update(ff, labelled, save_path, detect_xmp_sidecar(save_path));
		const auto ps = scanned.to_props();
		assert_equal(label_approved_text, ps->label, "label written in place to emoji-named file");
		assert_equal(3, ps->rating, "existing rating survives the in-place replacement");
	}

	metadata_edits clear_edits;
	clear_edits.remove_rating = true;
	clear_edits.label = std::string{};
	auto cleared = ff.update(save_path, clear_edits, {}, {}, false, {}, ff_inspect_rescan(save_path));

	{
		const auto scanned = ff_scan_after_update(ff, cleared, save_path, detect_xmp_sidecar(save_path));
		const auto ps = scanned.to_props();
		assert_equal(0, ps->rating, "rating cleared from emoji-named file");
		assert_equal(std::string_view{}, ps->label, "label cleared from emoji-named file");
	}

	// A failed replacement leaves the working file behind, which is how the reporter saw the
	// original name "disappear"; only the emoji-named file may remain.
	assert_equal(true, platform::exists(save_path), "emoji-named file survives every update");
	assert_equal(1, static_cast<int>(platform::iterate_file_items(folder, false).files.size()),
	             "no temporary files left beside the emoji-named file");
}

// The Date tool exists to correct a file whose capture time is wrong, and Test.jpg carries an EXIF
// DateTimeOriginal of its own. Writing only photoshop:DateCreated - the lowest-authority capture
// source - would leave that EXIF tag winning, and the tool would silently do nothing (#184).
static void should_write_an_edited_original_date()
{
	const auto load_path = test_files_folder().combine_file("Test.jpg");
	const auto save_path = test_temps.next_path(".jpg");

	files ff;
	const auto before = ff_scan_file(ff, load_path).to_props();
	assert_equal(core::date_t(2012, 9, 14, 19, 21, 14), before->dates.original(), "the file starts with its own date");

	metadata_edits edits;
	edits.date_original = core::date_t(1980, 6, 1, 9, 30, 0);

	auto written = ff.update(load_path, save_path, edits, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));
	assert_equal(true, written.success(), std::format("date written ({})", written.format_error()));

	const auto after = ff_scan_after_update(ff, written, save_path).to_props();
	assert_equal(core::date_t(1980, 6, 1, 9, 30, 0), after->dates.original(),
	             "the edited date is what the file reports");
	assert_equal(core::date_t(1980, 6, 1, 9, 30, 0), after->created(), "and it is the date the item shows");
}

// The same rule on the Created axis: Test.jpg carries its own EXIF DateTimeDigitized, which outranks
// xmp:CreateDate, so writing only the XMP property would leave that tag winning and the edit would
// silently do nothing. Original must survive it - correcting when a file was made says nothing about
// when the picture was taken.
static void should_write_an_edited_created_date()
{
	const auto load_path = test_files_folder().combine_file("Test.jpg");
	const auto save_path = test_temps.next_path(".jpg");

	files ff;
	const auto before = ff_scan_file(ff, load_path).to_props();
	assert_equal(core::date_t(2012, 9, 14, 19, 21, 14), before->dates.created(), "the file starts with its own date");

	metadata_edits edits;
	edits.date_created = core::date_t(1994, 3, 7, 14, 5, 0);

	auto written = ff.update(load_path, save_path, edits, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));
	assert_equal(true, written.success(), std::format("date written ({})", written.format_error()));

	const auto after = ff_scan_after_update(ff, written, save_path).to_props();
	assert_equal(core::date_t(1994, 3, 7, 14, 5, 0), after->dates.created(),
	             "the edited date is what the file reports");
	assert_equal(before->dates.original(), after->dates.original(), "an edit to one date leaves the others alone");
}

static void should_update_exif_rating()
{
	const auto load_path = test_files_folder().combine_file("exif-rating.jpg");
	const auto save_path = test_temps.next_path(".jpg");

	files ff;
	metadata_edits edits1;
	edits1.rating = 3;

	auto set_rating = ff.update(load_path, save_path, edits1, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));

	{
		const auto actual_scanned = ff_scan_after_update(ff, set_rating, save_path);
		const auto ps = actual_scanned.to_props();
		assert_equal(3, ps->rating, "to_props");

		const auto actual_exif = extract_properties(save_path, metadata_type::EXIF);
		const auto actual_iptc = extract_properties(save_path, metadata_type::IPTC);
		const auto actual_xmp = extract_properties(save_path, metadata_type::XMP);

		assert_equal(3, actual_xmp->rating, "XMP");
		assert_equal(3, actual_exif->rating, "exif");
	}

	metadata_edits edits2;
	edits2.remove_rating = true;
	auto clear_rating = ff.update(save_path, edits2, {}, {}, false, {}, ff_inspect_rescan(save_path));

	{
		const auto actual_scanned = ff_scan_after_update(ff, clear_rating, save_path);
		assert_equal(0, actual_scanned.to_props()->rating);

		const auto actual_exif = extract_properties(save_path, metadata_type::EXIF);
		const auto actual_iptc = extract_properties(save_path, metadata_type::IPTC);
		const auto actual_xmp = extract_properties(save_path, metadata_type::XMP);

		assert_equal(0, actual_xmp->rating, "XMP");
		assert_equal(0, actual_exif->rating, "exif");
		assert_equal(0, actual_iptc->rating, "IPTC");
	}
}

static void should_update_formatted_text()
{
	const auto load_path = test_files_folder().combine_file("exif-rating.jpg");
	const auto save_path = test_temps.next_path(".jpg");
	constexpr auto desc_text = "a\tb\nc";

	files ff;
	metadata_edits edits1;
	edits1.description = desc_text;

	auto described = ff.update(load_path, save_path, edits1, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));

	{
		const auto actual_scanned = ff_scan_after_update(ff, described, save_path);
		const auto ps = actual_scanned.to_props();
		assert_equal(desc_text, ps->description, "to_props");

		const auto actual_exif = extract_properties(save_path, metadata_type::EXIF);
		const auto actual_iptc = extract_properties(save_path, metadata_type::IPTC);
		const auto actual_xmp = extract_properties(save_path, metadata_type::XMP);

		assert_equal(desc_text, actual_xmp->description, "XMP");
		assert_equal(desc_text, actual_exif->description, "exif");
		assert_equal(desc_text, actual_iptc->description, "IPTC");
	}
}

static void should_update_synopsis()
{
	const auto load_path = test_files_folder().combine_file("exif-rating.jpg");
	const auto save_path = test_temps.next_path(".jpg");
	constexpr auto synopsis_text = "A long description of what happens";

	files ff;
	metadata_edits edits;
	edits.synopsis = synopsis_text;
	assert_equal(true, edits.has_changes(), "synopsis counts as a change");

	auto written = ff.update(load_path, save_path, edits, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));
	assert_equal(true, written.success(), std::format("synopsis written ({})", written.format_error()));

	const auto actual_scanned = ff_scan_after_update(ff, written, save_path);
	assert_equal(synopsis_text, actual_scanned.to_props()->synopsis, "to_props");

	const auto actual_xmp = extract_properties(save_path, metadata_type::XMP);
	assert_equal(synopsis_text, actual_xmp->synopsis, "XMP");
}

static void should_update_metadata(const std::string_view name)
{
	files ff;

	const auto ext = name.substr(core::find_ext(name));
	const auto save_path = test_temps.next_path(ext);
	const auto load_path = test_files_folder().combine_file(name);

	const auto tags_to_add = tag_set("key1 key2 key3");

	metadata_edits metadata_edits;
	metadata_edits.add_tags = tags_to_add;
	metadata_edits.copyright_notice = "Copyright xx";
	metadata_edits.rating = 3;
	metadata_edits.title = "Title xx";
	metadata_edits.description = "Description xx";

	auto written = ff.update(load_path, save_path, metadata_edits, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));

	const auto sr_expected = ff_scan_file(ff, load_path, detect_xmp_sidecar(load_path));
	const auto sr_actual = ff_scan_after_update(ff, written, save_path, detect_xmp_sidecar(save_path));

	const auto expected = sr_expected.to_props();
	const auto actual = sr_actual.to_props();

	expected->title = "Title xx"_c;
	expected->copyright_notice = "Copyright xx"_c;
	expected->description = "Description xx"_c;
	expected->rating = 3;
	expected->tags = make_unique_tags(tag_set(expected->tags), tags_to_add);

	reconcile_write_mutable_dates(*expected, *actual, "metadata");
	assert_metadata(*expected, *actual);
}

static void should_preserve_creator_names_with_apostrophes()
{
	files ff;

	const auto load_path = test_files_folder().combine_file("Test.jpg");

	const auto assert_creator_round_trip =
		[&ff, load_path](const std::string_view input, const std::vector<std::string>& creator_names)
	{
		const auto save_path = test_temps.next_path(".jpg");

		metadata_edits edits;
		edits.copyright_creator = input;
		auto written = ff.update(load_path, save_path, edits, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));
		assert_equal(true, written.success(), std::format("creator written ({})", written.format_error()));

		const auto scanned = ff_scan_after_update(ff, written, save_path, detect_xmp_sidecar(save_path));
		const auto props = scanned.to_props();
		const auto expected_creator = core::combine(creator_names);
		assert_equal(expected_creator, props->copyright_creator, "creator array read-back");

		const auto read_back_names = core::split(props->copyright_creator, true);
		assert_equal(creator_names.size(), read_back_names.size(), "creator array boundary count");
		for (auto i = 0_z; i < creator_names.size(); ++i)
		{
			assert_equal(creator_names[i], read_back_names[i], "creator array item");
		}
	};

	assert_creator_round_trip("Jane O'Brien; Alex; \"Smith; Pat\"", {"Jane O'Brien", "Alex", "Smith; Pat"});
	assert_creator_round_trip("\"Weird Al\" Yankovic; Madonna", {"\"Weird Al\" Yankovic", "Madonna"});
	assert_creator_round_trip("\"Doe, Jane\" ; Alex", {"Doe, Jane", "Alex"});
	assert_creator_round_trip("Jane Doe; 'Bud' Abbott; Lou Costello", {"Jane Doe", "'Bud' Abbott", "Lou Costello"});
	assert_creator_round_trip("\"Doe, Jane\" ", {"Doe, Jane"});
	assert_creator_round_trip("Alex; 'Til Tuesday; Bob", {"Alex", "'Til Tuesday", "Bob"});
}

static void should_add_remove_tags(const std::string_view name)
{
	const auto ext = name.substr(core::find_ext(name));
	const auto save_path = test_temps.next_path(ext);
	const auto load_path = test_files_folder().combine_file(name);

	const auto tags_to_add = tag_set("Максим yyyy zzzz");
	const auto tags_to_remove = tag_set("Максим zzzz");

	metadata_edits edits1;
	edits1.add_tags = tags_to_add;

	files ff;
	auto add_result = ff.update(load_path, save_path, edits1, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));
	assert_equal(true, add_result.success(), std::format("tags added ({})", add_result.format_error()));

	const auto sr_expected = ff_scan_file(ff, load_path, detect_xmp_sidecar(load_path));
	const auto sr_actual = ff_scan_after_update(ff, add_result, save_path, detect_xmp_sidecar(save_path));

	const auto expected = sr_expected.to_props();
	const auto actual = sr_actual.to_props();

	expected->tags = make_unique_tags(tag_set(expected->tags), tags_to_add);

	reconcile_write_mutable_dates(*expected, *actual, "tags added");
	assert_metadata(*expected, *actual, "added");

	metadata_edits edits2;
	edits2.remove_tags = tags_to_remove;
	auto remove_result = ff.update(save_path, edits2, {}, {}, false, {}, ff_inspect_rescan(save_path));
	assert_equal(true, remove_result.success(), std::format("tags removed ({})", remove_result.format_error()));

	tag_set expected_tags(expected->tags);
	expected_tags.remove(tags_to_remove);
	expected->tags = make_unique_tags(expected_tags, {});

	const auto sr_actual2 = ff_scan_after_update(ff, remove_result, save_path, detect_xmp_sidecar(save_path));
	const auto actual2 = sr_actual2.to_props();
	reconcile_write_mutable_dates(*expected, *actual2, "tags removed");
	assert_metadata(*expected, *actual2, "removed");
}

#ifdef _WIN32
static void should_remove_shell_written_tags()
{
	// Copy a test JPEG to temp so we can modify it
	const auto load_path = test_files_folder().combine_file("Test.jpg");
	const auto save_path = test_temps.next_path(".jpg");

	files ff;
	ff.update(load_path, save_path, {}, {}, {}, false, {});

	// Write tags via Windows shell properties (simulates Windows Explorer tag editing)
	const std::vector<std::string> shell_tags = {"ShellTag1", "ShellTag2", "ShellTag3"};
	const auto write_result = platform::write_shell_tags(save_path, shell_tags);
	assert_equal(true, write_result.success(), std::format("write_shell_tags ({})", write_result.format_error()));

	// Verify shell tags are readable via shell
	{
		const auto shell_meta = platform::read_shell_metadata(save_path);
		assert_equal(3u, static_cast<unsigned>(shell_meta.tags.size()), "shell read count");
	}

	// Verify Diffractor can read the shell-written tags
	tag_set baseline_tags;
	{
		const auto sr = ff_scan_file(ff, save_path);
		const auto ps = sr.to_props();
		baseline_tags = tag_set(ps->tags);
		assert_equal(true, baseline_tags.size() >= 3u, "diffractor read shell tags");
	}

	// Remove one tag via Diffractor
	{
		metadata_edits edits;
		edits.remove_tags = tag_set("ShellTag2");
		const auto remove_result = ff.update(save_path, edits, {}, {}, false, {});
		assert_equal(true, remove_result.success(), std::format("remove ShellTag2 ({})", remove_result.format_error()));
	}

	// Verify the tag is removed from Diffractor's perspective
	{
		const auto sr = ff_scan_file(ff, save_path);
		const auto ps = sr.to_props();
		const tag_set remaining(ps->tags);

		auto expected = baseline_tags;
		expected.remove(tag_set("ShellTag2"));
		assert_equal_strict(expected.to_string(" ", false), remaining.to_string(" ", false),
		                    "only ShellTag2 is removed from Diffractor tags");
	}

	// Verify tags are also removed from shell perspective (IPTC/EXIF)
	{
		const auto shell_meta = platform::read_shell_metadata(save_path);
		assert_equal_strict(tag_set("ShellTag1 ShellTag3").to_string(" ", false),
		                    tag_set(shell_meta.tags).to_string(" ", false),
		                    "only ShellTag2 is removed from shell tags");
	}

	// Remove all remaining tags
	{
		metadata_edits edits;
		edits.remove_tags = tag_set("ShellTag1 ShellTag3");
		const auto remove_result = ff.update(save_path, edits, {}, {}, false, {});
		assert_equal(true, remove_result.success(),
		             std::format("remove remaining shell tags ({})", remove_result.format_error()));
	}

	// Verify all tags are gone
	{
		const auto sr = ff_scan_file(ff, save_path);
		const auto ps = sr.to_props();
		auto expected = baseline_tags;
		expected.remove(tag_set("ShellTag1 ShellTag2 ShellTag3"));
		assert_equal_strict(expected.to_string(" ", false), tag_set(ps->tags).to_string(" ", false),
		                    "unrelated baseline tags survive");

		const auto shell_meta = platform::read_shell_metadata(save_path);
		assert_equal_strict(tag_set().to_string(" ", false), tag_set(shell_meta.tags).to_string(" ", false),
		                    "all shell-written tags removed");
	}
}
#endif

// libfiles reads the Windows Explorer / Media Player tags (WM/Category) and user rating
// (WM/SharedUserRating) from an MP4 or QuickTime file's 'Xtra' box, where it also writes them. The
// committed fixtures carry these in-file, so they read without the Shell API. (#123)
static void should_read_mp4_xtra_metadata(const std::string_view name, const std::string_view expect_tag)
{
	files ff;
	const auto sr = ff_scan_file(ff, test_files_folder().combine_file(name));
	assert_equal(true, sr.media != nullptr, std::format("libfiles read {}", name));
	if (!sr.media) return;

	bool found_category = false;
	bool found_rating = false;
	for (const auto& tag : sr.media->tags)
	{
		if (core::icmp(tag.key, "WM/Category") == 0 && tag.value.find(expect_tag) != std::string::npos)
			found_category = true;
		if (core::icmp(tag.key, "WM/SharedUserRating") == 0) found_rating = true;
	}

	const auto& keywords = sr.media->properties.keywords;
	const auto keyword = std::ranges::any_of(keywords, [expect_tag](const std::string& k)
	{ return k.find(expect_tag) != std::string::npos; });

	assert_equal(true, found_category, std::format("xtra category tag {}", name));
	assert_equal(true, keyword, std::format("xtra keywords {}", name));
	assert_equal(true, found_rating, std::format("xtra rating {}", name));
	assert_equal(true, sr.media->properties.rating.has_value(), std::format("xtra rating property {}", name));
}

#ifdef _WIN32
// An ASF file's Windows 'WM/Category' keywords all read, not just the first. Windows stores them as
// separate descriptors across the Extended Content Description and Metadata Library objects, and
// libfiles writes them as separate descriptors too. Tag a WMV, then read it back. (#123)
static void should_read_asf_wm_categories()
{
	const auto load_path = test_files_folder().combine_file("gen.wmv");
	const auto save_path = test_temps.next_path(".wmv");

	files ff;
	const auto copy_result = ff.update(load_path, save_path, {}, {}, {}, false, {});
	assert_equal(true, copy_result.success(), std::format("asf copy written ({})", copy_result.format_error()));

	metadata_edits edits;
	edits.add_tags = tag_set("AsfTagA AsfTagB AsfTagC");
	const auto tag_result = ff.update(save_path, edits, {}, {}, false, {}, ff_inspect_rescan(save_path));
	assert_equal(true, tag_result.success(), std::format("asf tags written ({})", tag_result.format_error()));

	const auto sr = ff_scan_file(ff, save_path);
	assert_equal(true, sr.media != nullptr, "libfiles read the asf");
	if (!sr.media) return;

	const auto& keywords = sr.media->properties.keywords;
	const auto has = [&keywords](const std::string_view k)
	{
		return std::ranges::find(keywords, k) != keywords.end();
	};

	assert_equal(true, has("AsfTagA"), "asf WM/Category A");
	assert_equal(true, has("AsfTagB"), "asf WM/Category B");
	assert_equal(true, has("AsfTagC"), "asf WM/Category C");
}

// Windows compatibility (write): Diffractor writes tags & rating via its normal
// XMP update; libfiles reconciles them into the Windows 'Xtra' atom, so the
// Windows Shell (Explorer / Media Player) reads them back. Validates that libfiles
// writes Microsoft's binary format byte-exactly. (#123)
static void should_write_windows_tags_via_xmp(const std::string_view name)
{
	const auto ext = name.substr(core::find_ext(name));
	const auto load_path = test_files_folder().combine_file(name);
	const auto save_path = test_temps.next_path(ext);
	const auto original_subtitle = find_xtra_entry(load_path, "WM/SubTitle");

	metadata_edits edits;
	edits.add_tags = tag_set("WinWriteA WinWriteB");
	edits.rating = 4;

	files ff;
	ff.update(load_path, save_path, edits, {}, {}, false, {});

	const auto shell = platform::read_shell_metadata(save_path);

	bool found_a = false, found_b = false;
	for (const auto& t : shell.tags)
	{
		if (core::icmp(t, "WinWriteA") == 0) found_a = true;
		if (core::icmp(t, "WinWriteB") == 0) found_b = true;
	}
	assert_equal(true, found_a && found_b, std::format("shell reads xmp-written tags {}", name));
	assert_equal(true, shell.rating.has_value() && *shell.rating >= 63,
	             std::format("shell reads xmp-written rating {}", name)); // 4 stars => 75

	// The file must remain valid (not corrupted by the native-metadata write).
	const auto sr = ff_scan_file(ff, save_path);
	assert_equal(true, sr.success, std::format("file still scans after write {}", name));
	if (!original_subtitle.empty())
	{
		assert_equal(true, original_subtitle == find_xtra_entry(save_path, "WM/SubTitle"),
		             std::format("unrelated Xtra subtitle preserved {}", name));
	}
}

// Windows compatibility: the tags Windows Explorer / Media Player write live in
// the Shell property system (MP4 'Xtra' atom, ASF 'WM/*' descriptors), which
// FFmpeg does not fully surface. Verify Diffractor's Shell wrappers can write and
// read them back the same way Windows does, for the formats Windows can tag. (#123)
static void should_roundtrip_windows_shell_tags(const std::string_view name)
{
	const auto ext = name.substr(core::find_ext(name));
	const auto load_path = test_files_folder().combine_file(name);
	const auto save_path = test_temps.next_path(ext);

	files ff;
	ff.update(load_path, save_path, {}, {}, {}, false, {}); // copy to temp

	const std::vector<std::string> shell_tags = {"WinTagA", "WinTagB", "Максим"};
	const auto write_result = platform::write_shell_tags(save_path, shell_tags);
	assert_equal(true, write_result.success(),
	             std::format("write_shell_tags {} ({})", name, write_result.format_error()));

	// The same Shell property store Explorer uses reads the tags back verbatim.
	const auto shell_meta = platform::read_shell_metadata(save_path);
	assert_equal(3u, static_cast<unsigned>(shell_meta.tags.size()), std::format("shell tag count {}", name));

	bool found_a = false, found_b = false, found_c = false;
	for (const auto& t : shell_meta.tags)
	{
		if (core::icmp(t, "WinTagA") == 0) found_a = true;
		else if (core::icmp(t, "WinTagB") == 0) found_b = true;
		else if (core::icmp(t, "Максим") == 0) found_c = true;
	}
	assert_equal(true, found_a && found_b && found_c, std::format("shell tags round-trip {}", name));

	// Diffractor's scan must still succeed on the Windows-tagged file.
	const auto sr = ff_scan_file(ff, save_path);
	assert_equal(true, sr.success, std::format("scan ok {}", name));
}
#endif

// Containers Windows cannot tag (no property handler — AVI, MPEG program stream):
// the Shell write must fail gracefully and the Diffractor scan must still succeed.
static void should_handle_unsupported_shell_tags(const std::string_view name)
{
	const auto ext = name.substr(core::find_ext(name));
	const auto load_path = test_files_folder().combine_file(name);
	const auto save_path = test_temps.next_path(ext);

	files ff;
	ff.update(load_path, save_path, {}, {}, {}, false, {});

	const auto write_result = platform::write_shell_tags(save_path, {"X", "Y"});
	assert_equal(false, write_result.success(), std::format("shell write unsupported {}", name));

	const auto sr = ff_scan_file(ff, save_path);
	assert_equal(true, sr.success, std::format("scan still ok {}", name));
}

static void should_update_location(const std::string_view name)
{
	files ff;

	const auto ext = name.substr(core::find_ext(name));
	const auto save_path = test_temps.next_path(ext);
	const auto load_path = test_files_folder().combine_file(name);
	const auto coordinate = gps_coordinate(40.71417, -74.00611);

	metadata_edits metadata_edits;
	metadata_edits.location_coordinate = coordinate;
	metadata_edits.location_place = "Big Apple"_c;
	metadata_edits.location_state = "New York"_c;
	metadata_edits.location_country = "USA"_c;

	auto written = ff.update(load_path, save_path, metadata_edits, {}, {}, false, {}, {}, ff_inspect_rescan(save_path));

	const auto sr_expected = ff_scan_file(ff, load_path, detect_xmp_sidecar(load_path));
	const auto sr_actual = ff_scan_after_update(ff, written, save_path, detect_xmp_sidecar(save_path));

	const auto expected = sr_expected.to_props();
	const auto actual = sr_actual.to_props();

	expected->coordinate = coordinate;
	expected->location_place = "Big Apple"_c;
	expected->location_state = "New York"_c;
	expected->location_country = "USA"_c;

	reconcile_write_mutable_dates(*expected, *actual, "location");
	assert_metadata(*expected, *actual);
}

static void should_update_gps_in_exif()
{
	const auto save_path = test_temps.next_path(".jpg");
	const auto load_path = test_files_folder().combine_file("IMG_9340.JPG");
	const auto coordinate = gps_coordinate(40.71417, -74.00611);

	metadata_edits metadata_edits;
	metadata_edits.location_coordinate = coordinate;
	metadata_edits.location_place = "Big Apple"_c;
	metadata_edits.location_state = "New York"_c;
	metadata_edits.location_country = "USA"_c;

	files ff;
	ff.update(load_path, save_path, metadata_edits, {}, {}, false, {});

	const auto actual_exif = extract_properties(save_path, metadata_type::EXIF);
	const auto actual_iptc = extract_properties(save_path, metadata_type::IPTC);
	const auto actual_xmp = extract_properties(save_path, metadata_type::XMP);

	assert_equal("Big Apple", actual_iptc->location_place, "IPTC");
	assert_equal("New York", actual_iptc->location_state, "IPTC");
	assert_equal("USA", actual_iptc->location_country, "IPTC");
	assert_equal({40.71417, -74.00611}, actual_exif->coordinate, "EXIF");

	assert_equal("Big Apple", actual_xmp->location_place, "XMP");
	assert_equal("New York", actual_xmp->location_state, "XMP");
	assert_equal("USA", actual_xmp->location_country, "XMP");
}

static void should_handle_international_characters()
{
	const auto save_path = test_temps.next_path(".jpg");
	const auto load_path = test_files_folder().combine_file("Test.jpg");
	constexpr auto description = "In vollen Zügen genießen";

	tag_set tags;
	tags.add_one("In vollen Zügen genießen");
	tags.add_one("Nældens takvinge");
	tags.add_one("Žižkov");

	const auto test = core::utf16_to_utf8(core::utf8_to_utf16(description));
	assert_equal(description, test);

	metadata_edits edits;
	edits.description = description;
	edits.remove_tags = tag_set("key1 key2 key3");
	edits.add_tags = tags;

	files ff;
	ff.update(load_path, save_path, edits, {}, {}, false, {});

	const auto actual_exif = extract_properties(save_path, metadata_type::EXIF);
	const auto actual_iptc = extract_properties(save_path, metadata_type::IPTC);
	const auto actual_xmp = extract_properties(save_path, metadata_type::XMP);

	assert_equal(description, actual_xmp->description, "XMP");
	assert_equal(description, actual_exif->description, "EXIF");
	assert_equal(description, actual_iptc->description, "IPTC");

	const auto expected_tags = tags.to_string();
	assert_equal(expected_tags, actual_xmp->tags, "XMP Tags");
	assert_equal(expected_tags, actual_iptc->tags, "IPTC Tags");

	const auto sr = ff_scan_file(ff, save_path);
	const auto ps = sr.to_props();
	assert_equal(description, ps->description);
	assert_equal(expected_tags, ps->tags, "Tags");
}

// Issue #219 - Korean (Hangul) metadata must survive a write/read round-trip
// through IPTC and XMP the same as other non-Latin scripts. Uses \u escapes so
// the source file stays ASCII-clean.
static void should_handle_korean_characters()
{
	const auto save_path = test_temps.next_path(".jpg");
	const auto load_path = test_files_folder().combine_file("Test.jpg");
	// "서울에서 찍은 사진" (a photo taken in Seoul)
	constexpr auto description = "\uC11C\uC6B8\uC5D0\uC11C \uCC0D\uC740 \uC0AC\uC9C4";

	tag_set tags;
	tags.add_one("\uAC00\uC871"); // 가족  family
	tags.add_one("\uC5EC\uD589"); // 여행  travel
	tags.add_one("\uC11C\uC6B8"); // 서울  Seoul
	tags.make_unique();           // tags are stored/read back in sorted order

	// UTF-8 <-> UTF-16 round-trip of the Korean text must be lossless.
	const auto test = core::utf16_to_utf8(core::utf8_to_utf16(description));
	assert_equal(description, test);

	metadata_edits edits;
	edits.description = description;
	edits.remove_tags = tag_set("key1 key2 key3"); // test.jpg ships with these
	edits.add_tags = tags;

	files ff;
	ff.update(load_path, save_path, edits, {}, {}, false, {});

	const auto actual_xmp = extract_properties(save_path, metadata_type::XMP);
	const auto actual_iptc = extract_properties(save_path, metadata_type::IPTC);

	assert_equal(description, actual_xmp->description, "XMP");
	assert_equal(description, actual_iptc->description, "IPTC");

	const auto expected_tags = tags.to_string();
	assert_equal(expected_tags, actual_xmp->tags, "XMP Tags");
	assert_equal(expected_tags, actual_iptc->tags, "IPTC Tags");

	// Full scan (the path used by the index) must also read the Korean values.
	const auto sr = ff_scan_file(ff, save_path);
	const auto ps = sr.to_props();
	assert_equal(description, ps->description, "scan description");
	assert_equal(expected_tags, ps->tags, "scan tags");
}

static void should_edit_video_metadata_in_place(const std::string_view name)
{
	const auto source = test_files_folder().combine_file(name);
	const auto path = test_temps.next_path(source.extension());
	platform::copy_file(source, path, false, false);

	files ff;

	metadata_edits first;
	first.rating = 3;
	assert_equal(true, ff.update(path, first, {}, {}, false, {}).success(), "first rating saved");
	assert_equal(false, ff_scan_file(ff, path).metadata.xmp.empty(), "packet embedded");

	const auto size_before = platform::file_attributes(path).size;

	// A rating that fits the packet is patched where the file lies, so the file is never staged
	// and swapped, which proves the whole media file was not copied.
	metadata_edits second;
	second.rating = 5;
	const auto result = ff.update(path, second, {}, {}, false, {});

	assert_equal(true, result.success(), "second rating saved");
	assert_equal(false, result.staged, "second rating edited in place");
	assert_equal(true, result.created_files.files.empty(), "no staged files left behind");

	const auto sr = ff_scan_file(ff, path);
	assert_equal(5, sr.to_props()->rating, "rating round trips");
	assert_equal(size_before, platform::file_attributes(path).size, "file not rewritten");
}

// ISO base media absorbs a first packet with a bounded box write, so the opening rating of an
// import must not copy the whole video.
static void should_inject_video_metadata_in_place(const std::string_view name)
{
	const auto source = test_files_folder().combine_file(name);
	const auto path = test_temps.next_path(source.extension());
	platform::copy_file(source, path, false, false);

	files ff;
	assert_equal(true, ff_scan_file(ff, path).metadata.xmp.empty(), "starts with no packet");

	metadata_edits edits;
	edits.rating = 4;
	const auto result = ff.update(path, edits, {}, {}, false, {});

	assert_equal(true, result.success(), "first rating saved");
	assert_equal(false, result.staged, "first rating injected in place");
	assert_equal(true, result.created_files.files.empty(), "no staged files left behind");

	const auto sr = ff_scan_file(ff, path);
	assert_equal(4, sr.to_props()->rating, "rating round trips");
}

// Every format libfiles writes takes an edit that fits the space the file holds where it lies. The
// first edit leaves room - a padded packet, the EXIF rating fields, ID3 padding - so the second is a
// patch: the file is not staged and swapped, and keeps its size.
static void should_patch_a_metadata_edit_that_fits(const std::string_view name)
{
	const auto source = test_files_folder().combine_file(name);
	const auto path = test_temps.next_path(source.extension());
	platform::copy_file(source, path, false, false);

	files ff;

	metadata_edits first;
	first.rating = 3;
	const auto first_result = ff.update(path, first, {}, {}, false, {});
	assert_equal(true, first_result.success(), std::format("first rating saved ({})", first_result.format_error()));
	assert_equal(true, first_result.created_files.files.empty(), "no staged files left behind");

	const auto size_before = platform::file_attributes(path).size;

	metadata_edits second;
	second.rating = 5;
	const auto result = ff.update(path, second, {}, {}, false, {});

	assert_equal(true, result.success(), std::format("second rating saved ({})", result.format_error()));
	assert_equal(false, result.staged, "the second rating is patched where the file lies");
	assert_equal(true, result.created_files.files.empty(), "no staged files left behind");
	assert_equal(size_before, platform::file_attributes(path).size, "nothing the file holds moved");

	const auto sr = ff_scan_file(ff, path, detect_xmp_sidecar(path));
	assert_equal(5, sr.to_props()->rating, "rating round trips");
}

// An edit that does not fit - a JPEG's first packet moves the image after it - is staged and swapped
// in, so the original survives a failure.
static void should_stage_a_metadata_edit_that_does_not_fit()
{
	const auto path = test_temps.next_path(".jpg");
	platform::copy_file(test_files_folder().combine_file("IMG_9340.JPG"), path, false, false);
	const auto size_before = platform::file_attributes(path).size;

	files ff;
	metadata_edits edits;
	edits.rating = 2;
	const auto result = ff.update(path, edits, {}, {}, false, {});

	assert_equal(true, result.success(), std::format("rating saved ({})", result.format_error()));
	assert_equal(true, result.staged, "edit staged and swapped");
	assert_equal(true, result.created_files.files.empty(), "no staged files left behind");
	assert_equal(true, platform::file_attributes(path).size != size_before, "the packet moved the image");

	const auto sr = ff_scan_file(ff, path);
	assert_equal(2, sr.to_props()->rating, "rating round trips");
}

// A staged edit holds the same bytes however it is staged: one pass from the source when the edit moves
// what the file holds, a copy patched where it fits. Each is held to what copying the file and updating
// the copy writes, and a file libfiles does not write is left to the caller, with nothing staged.
static void should_stage_an_edit_as_copying_and_updating_would()
{
	const auto source = test_temps.next_path(".jpg");
	platform::copy_file(test_files_folder().combine_file("IMG_9340.JPG"), source, false, false);

	const auto copied_and_updated = [](const core::file_path from, const metadata_edits& edits)
	{
		const auto path = test_temps.next_path(".jpg");
		platform::copy_file(from, path, false, false);
		assert_equal(true, metadata_xmp::update(path, from, edits, {}, {}).success, "copy updated");
		return platform::blob_from_file(path);
	};

	// The file has no packet, so its first one moves the image after it.
	metadata_edits first;
	first.rating = 2;
	first.title = "Staged in one pass";
	const auto first_expected = copied_and_updated(source, first);
	const auto first_stage = test_temps.next_path(".jpg");
	const auto first_staged = metadata_xmp::stage_update(first_stage, source, first, {});
	assert_equal(true, first_staged.has_value() && first_staged->success, "first edit staged");

	const auto first_bytes = platform::blob_from_file(first_stage);
	assert_equal(true, first_bytes.size() != platform::file_attributes(source).size,
	             "the first packet moves the image");
	assert_equal(true, std::ranges::equal(first_expected, first_bytes),
	             "one pass writes what a copy and an update write");

	// The second fits the packet and the EXIF fields the first wrote.
	metadata_edits second;
	second.rating = 4;
	const auto second_expected = copied_and_updated(first_stage, second);
	const auto second_stage = test_temps.next_path(".jpg");
	const auto second_staged = metadata_xmp::stage_update(second_stage, first_stage, second, {});
	assert_equal(true, second_staged.has_value() && second_staged->success, "second edit staged");

	const auto second_bytes = platform::blob_from_file(second_stage);
	assert_equal(first_bytes.size(), second_bytes.size(), "an edit that fits is a patch");
	assert_equal(true, std::ranges::equal(second_expected, second_bytes), "a patched copy writes the same");

	files ff;
	const auto scanned = ff_scan_file(ff, second_stage);
	assert_equal(4, scanned.to_props()->rating, "the edit reads back");
	assert_equal("Staged in one pass"_c, scanned.to_props()->title, "with what the first edit wrote");

	// A raw file keeps its metadata in a sidecar.
	const auto raw = test_temps.next_path(".CR2");
	platform::copy_file(test_files_folder().combine("raw").combine_file("Screws.CR2"), raw, false, false);
	const auto raw_stage = test_temps.next_path(".CR2");
	assert_equal(false, metadata_xmp::stage_update(raw_stage, raw, first, {}).has_value(),
	             "a raw file is the caller's");
	assert_equal(false, platform::exists(raw_stage), "and nothing is staged for it");
}

// A raw file's metadata lives in its sidecar, so rating one must not cost the size of the raw.
// The media file is the assertion: an untouched size, modified time and byte count prove no copy
// or swap ran, and a sidecar-only write proves replace_file never saw the raw at all.
static void should_edit_raw_sidecar_only()
{
	const auto path = test_temps.next_path(".CR2");
	const auto path_xmp = path.extension(".xmp");
	const auto raw_folder = test_files_folder().combine("raw");

	platform::copy_file(raw_folder.combine_file("Screws.CR2"), path, false, false);
	platform::copy_file(raw_folder.combine_file("Screws.xmp"), path_xmp, false, false);

	const auto attributes_before = platform::file_attributes(path);
	const auto raw_before = platform::blob_from_file(path);

	files ff;
	metadata_edits edits;
	edits.rating = 5;
	const auto result = ff.update(path, edits, {}, {}, false, {});

	assert_equal(true, result.success(), "rating saved");
	assert_equal(false, result.staged, "raw not staged and swapped");
	assert_equal(true, result.created_files.files.empty(), "no staged files left behind");

	const auto attributes_after = platform::file_attributes(path);
	assert_equal(attributes_before.size, attributes_after.size, "raw size unchanged");
	assert_equal(attributes_before.modified, attributes_after.modified, "raw modified time unchanged");
	assert_equal(true, raw_before == platform::blob_from_file(path), "raw bytes unchanged");

	prop::item_metadata sidecar;
	metadata_xmp::parse(sidecar, path_xmp);
	assert_equal(5, sidecar.rating, "sidecar rating");

	const auto sr = ff_scan_file(ff, path, detect_xmp_sidecar(path));
	assert_equal(5, sr.to_props()->rating, "rating round trips");
}

// A RAW capture date corrected through the Date tool is written to the sidecar. The embedded RAW
// timestamp is still present on rescan, but it must not outrank the sidecar's corrected Original
// date or the tool appears to do nothing.
static void should_write_raw_sidecar_original_date()
{
	const auto path = test_temps.next_path(".CR2");
	const auto path_xmp = path.extension(".xmp");
	const auto raw_folder = test_files_folder().combine("raw");

	platform::copy_file(raw_folder.combine_file("Screws.CR2"), path, false, false);
	platform::copy_file(raw_folder.combine_file("Screws.xmp"), path_xmp, false, false);

	files ff;
	const auto before = ff_scan_file(ff, path, detect_xmp_sidecar(path)).to_props();
	assert_equal(core::date_t(2011, 9, 23, 23, 49, 16), before->dates.best(), "raw starts at embedded date");

	metadata_edits edits;
	edits.date_original = core::date_t(1980, 6, 1, 9, 30, 0);
	const auto written = ff.update(path, edits, {}, {}, false, {});
	assert_equal(true, written.success(), std::format("raw date saved ({})", written.format_error()));

	const auto after = ff_scan_file(ff, path, detect_xmp_sidecar(path)).to_props();
	assert_equal(core::date_t(1980, 6, 1, 9, 30, 0), after->dates.original(), "sidecar original date wins");
	assert_equal(core::date_t(1980, 6, 1, 9, 30, 0), after->dates.best(), "and is the item date");
}

static core::blob make_xmp_packet(const std::string_view body)
{
	const auto xml = std::format("<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
	                             "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
	                             "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
	                             "<rdf:Description rdf:about=\"\" "
	                             "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
	                             "xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">{}</rdf:Description>"
	                             "</rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>",
	                             body);
	return {std::bit_cast<const uint8_t*>(xml.data()), std::bit_cast<const uint8_t*>(xml.data() + xml.size())};
}

static void write_xmp_packet(const core::file_path path, const std::string_view body)
{
	platform::blob_save_to_file(make_xmp_packet(body), path);
}

static void set_raw_source_xmp(const core::file_path path)
{
	metadata_xmp::set_test_raw_source_xmp(
		path,
		make_xmp_packet("<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">Embedded title</rdf:li></rdf:Alt></dc:title>"
	                    "<dc:subject><rdf:Bag><rdf:li>embedded-tag</rdf:li></rdf:Bag></dc:subject>"));
}

static void should_seed_first_raw_sidecar_from_embedded_xmp()
{
	const auto path = test_temps.next_path(".CR2");
	const auto path_xmp = path.extension(".xmp");
	const auto raw_folder = test_files_folder().combine("raw");
	platform::copy_file(raw_folder.combine_file("Screws.CR2"), path, false, false);
	const auto raw_before = platform::blob_from_file(path);
	set_raw_source_xmp(path);

	metadata_edits edits;
	edits.rating = 4;
	files ff;
	const auto result = ff.update(path, edits, {}, {}, false, {});
	metadata_xmp::set_test_raw_source_xmp({}, {});

	assert_equal(true, result.success(), std::format("rating saved ({})", result.format_error()));
	assert_equal(true, platform::exists(path_xmp), "first sidecar created");
	assert_equal(true, raw_before == platform::blob_from_file(path), "raw bytes unchanged");

	prop::item_metadata sidecar;
	metadata_xmp::parse(sidecar, path_xmp);
	assert_equal("Embedded title", sidecar.title, "embedded title seeds sidecar");
	assert_equal("embedded-tag", sidecar.tags, "embedded tags seed sidecar");
	assert_equal(4, sidecar.rating, "edit applied");
}

static void should_use_existing_partial_raw_sidecar_as_source()
{
	const auto path = test_temps.next_path(".CR2");
	const auto path_xmp = path.extension(".xmp");
	const auto raw_folder = test_files_folder().combine("raw");
	platform::copy_file(raw_folder.combine_file("Screws.CR2"), path, false, false);
	write_xmp_packet(path_xmp,
	                 "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">Sidecar title</rdf:li></rdf:Alt></dc:title>");
	set_raw_source_xmp(path);

	metadata_edits edits;
	edits.rating = 2;
	files ff;
	const auto result = ff.update(path, edits, {}, {}, false, {});
	metadata_xmp::set_test_raw_source_xmp({}, {});

	assert_equal(true, result.success(), std::format("rating saved ({})", result.format_error()));
	prop::item_metadata sidecar;
	metadata_xmp::parse(sidecar, path_xmp);
	assert_equal("Sidecar title", sidecar.title, "existing sidecar title survives");
	assert_equal(true, core::is_empty(sidecar.tags), "partial sidecar remains authoritative over embedded tags");
	assert_equal(2, sidecar.rating, "edit applied");
}

static void should_keep_empty_raw_sidecar_subject_authoritative()
{
	const auto path = test_temps.next_path(".CR2");
	const auto path_xmp = path.extension(".xmp");
	const auto raw_folder = test_files_folder().combine("raw");
	platform::copy_file(raw_folder.combine_file("Screws.CR2"), path, false, false);
	write_xmp_packet(path_xmp, "<dc:subject><rdf:Bag/></dc:subject>");
	set_raw_source_xmp(path);

	metadata_edits edits;
	edits.rating = 1;
	files ff;
	const auto result = ff.update(path, edits, {}, {}, false, {});
	metadata_xmp::set_test_raw_source_xmp({}, {});

	assert_equal(true, result.success(), std::format("rating saved ({})", result.format_error()));
	prop::item_metadata sidecar;
	metadata_xmp::parse(sidecar, path_xmp);
	assert_equal(true, core::is_empty(sidecar.tags), "explicit empty sidecar subject stays authoritative");
	assert_equal(1, sidecar.rating, "edit applied");
}

static void should_force_rescan_after_failed_live_metadata_write()
{
	file_update_result untouched_failure;
	untouched_failure.code = platform::file_op_result_code::FAILED;
	assert_equal(false, failed_write_needs_forced_rescan(untouched_failure),
	             "ordinary failures keep existing failure reporting without a forced scan");

	file_update_result changed_failure;
	changed_failure.code = platform::file_op_result_code::FAILED;
	changed_failure.live_file_maybe_changed = true;
	assert_equal(true, failed_write_needs_forced_rescan(changed_failure),
	             "live-mutating failures are still failures and force a rescan");
	assert_equal(false, changed_failure.success(), "the failed result stays visible");
}

static void should_revalidate_folder_after_failed_live_date_write()
{
	file_update_result changed_failure;
	changed_failure.code = platform::file_op_result_code::FAILED;
	changed_failure.live_file_maybe_changed = true;
	changed_failure.error_message = "distinctive live write failure";

	std::string first_error;
	core::unique_folders written;
	const auto status = apply_batch_update_result(written, test_temps.folder(), first_error, changed_failure);
	assert_equal(true, item_status::fail == status, "date batches mark the item failed");
	assert_equal(true, written.contains(test_temps.folder()), "date batches revalidate a failed live-mutating write");
	assert_equal("distinctive live write failure", first_error, "date batches keep the failed write error");
}

static void should_mark_retry_failure_after_live_metadata_mutation()
{
	const auto source = test_files_folder().combine_file("gizmo.mp4");
	const auto path = test_temps.next_path(".mp4");
	platform::copy_file(source, path, false, false);

	metadata_edits first;
	first.rating = 3;
	files ff;
	assert_equal(true, ff.update(path, first, {}, {}, false, {}).success(), "packet embedded");

	metadata_edits second;
	second.rating = 4;
	metadata_xmp::set_test_update_failure_sequence({1, 2});
	const auto result = ff.update(path, second, {}, {}, false, {});
	metadata_xmp::set_test_update_failure_sequence({});

	assert_equal(false, result.success(), "retry failure is reported");
	assert_equal(true, result.live_file_maybe_changed, "retry mutation is carried for recovery scan");
}

// The source is made unreadable with a Windows share-mode lock; POSIX has no mandatory lock to
// hold it with, so the test is registered on Windows only.
#ifdef _WIN32
static void should_refuse_first_raw_sidecar_when_raw_source_is_unreadable()
{
	const auto path = test_temps.next_path(".CR2");
	const auto staged_path = test_temps.next_path(".xmp");
	const auto raw_folder = test_files_folder().combine("raw");
	platform::copy_file(raw_folder.combine_file("Screws.CR2"), path, false, false);

	metadata_edits edits;
	edits.rating = 5;

	const auto lock = platform::open_file(path, platform::file_open_mode::read_write);
	assert_equal(true, static_cast<bool>(lock), "raw locked");

	auto refused = false;
	try
	{
		metadata_xmp::update(path, path, edits, {}, staged_path);
	}
	catch (const std::exception&)
	{
		refused = true;
	}

	assert_equal(true, refused, "unreadable raw source is refused, not shadowed by a default sidecar");
	assert_equal(false, platform::exists(staged_path), "no default sidecar is staged");
}
#endif

// A sidecar that exists but cannot be read is unknown, not empty. Applying the edits to a default
// packet and swapping it over the file would replace every property the sidecar already holds, so
// the write has to fail instead. Asserted against the staged packet rather than the swap, because a
// swap over a locked file fails on its own account and would pass either way.
static void should_refuse_to_write_an_unreadable_sidecar()
{
	const auto path = test_temps.next_path(".CR2");
	const auto path_xmp = path.extension(".xmp");
	const auto staged_path = test_temps.next_path(".xmp");
	const auto raw_folder = test_files_folder().combine("raw");

	platform::copy_file(raw_folder.combine_file("Screws.CR2"), path, false, false);
	platform::copy_file(raw_folder.combine_file("Screws.xmp"), path_xmp, false, false);

	metadata_edits edits;
	edits.rating = 5;

#ifdef _WIN32
	{
		// read_write opens with no sharing, which is how a sidecar held by another application reads.
		// That denial is Windows' mandatory locking; a POSIX descriptor refuses nothing, so there is
		// no unreadable sidecar to present here.
		const auto lock = platform::open_file(path_xmp, platform::file_open_mode::read_write);
		assert_equal(true, static_cast<bool>(lock), "sidecar locked");

		auto refused = false;

		try
		{
			metadata_xmp::update(path, path, edits, {}, staged_path);
		}
		catch (const std::exception&)
		{
			refused = true;
		}

		assert_equal(true, refused, "an unreadable sidecar is refused, not rebuilt from the edits alone");
		assert_equal(false, platform::exists(staged_path), "and nothing is staged to swap over it");
	}
#endif

	const auto result = metadata_xmp::update(path, path, edits, {}, staged_path);
	assert_equal(true, result.success, "staged once the sidecar can be read");

	prop::item_metadata staged;
	metadata_xmp::parse(staged, staged_path);
	assert_equal(5, staged.rating, "the edit is applied");
	assert_equal(false, staged.title.is_empty(), "the properties already in the sidecar survive");
}

static void should_save_as_with_distinct_xmp_sidecar()
{
	const auto source_path = test_temps.next_path(".CR2");
	const auto source_xmp_path = source_path.extension(".xmp");
	const auto destination_path = test_temps.next_path(".CR2");
	const auto destination_xmp_path = destination_path.extension(".xmp");
	const auto raw_folder = test_files_folder().combine("raw");

	platform::copy_file(raw_folder.combine_file("Screws.CR2"), source_path, false, false);
	platform::copy_file(raw_folder.combine_file("Screws.xmp"), source_xmp_path, false, false);
	const auto source_xmp_before = platform::blob_from_file(source_xmp_path);

	metadata_edits edits;
	edits.title = "Saved as copy";

	files ff;
	const auto result = ff.update(source_path, destination_path, edits, {}, {}, false, source_xmp_path.name());

	assert_equal(true, result.success(), "save as succeeds");
	assert_equal(true, source_xmp_before == platform::blob_from_file(source_xmp_path), "source sidecar unchanged");
	assert_equal(true, platform::exists(destination_xmp_path), "destination sidecar created");
	prop::item_metadata destination_xmp;
	metadata_xmp::parse(destination_xmp, destination_xmp_path);
	assert_equal("Saved as copy", destination_xmp.title, "destination sidecar metadata");
}

// The rating in the packet a file keeps in itself, read where libfiles writes it.
static int own_packet_rating(const core::file_path path)
{
	const auto file = platform::blob_from_file(path);
	const std::vector<uint8_t> bytes(file.begin(), file.end());
	libfiles::reader r(bytes);
	const auto packet = libfiles::read_packet(r).packet;

	prop::item_metadata md;
	apply_properties(md, libfiles::properties_from_xmp(
							 std::span(std::bit_cast<const uint8_t*>(packet.data()), packet.size())));
	return md.rating;
}

// A DNG holds a packet of its own, but one with a sidecar beside it - darktable writes one for every
// image, under either name, and Camera Raw keeps a DNG's settings in one that is there - is read from the
// sidecar, so an edit is written there too. Written into the DNG it was read straight back from the
// sidecar and looked lost. The DNG is a TIFF here: Diffractor knows a DNG by its name, and libfiles
// writes one by what it holds, which for a DNG is a TIFF.
static void should_write_a_dng_sidecar_when_it_has_one()
{
	for (const auto darktable_name : {false, true})
	{
		const auto path = test_temps.next_path(".dng");
		platform::copy_file(test_files_folder().combine_file("Small.tif"), path, false, false);
		const auto path_xmp = darktable_name ? path.folder().combine_file(std::format("{}.xmp", path.name().sv()))
		                                     : path.extension(".xmp");
		write_xmp_packet(
			path_xmp, "<xmp:Rating>1</xmp:Rating>"
					  "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">Sidecar title</rdf:li></rdf:Alt></dc:title>");

		const auto dng_before = platform::blob_from_file(path);
		const auto modified_before = platform::file_attributes(path).modified;
		const auto what = darktable_name ? " (darktable's name)"s : " (Adobe's name)"s;

		metadata_edits edits;
		edits.rating = 4;
		files ff;
		const auto result = ff.update(path, edits, {}, {}, false, path_xmp.name());

		assert_equal(true, result.success(), std::format("rating saved ({}){}", result.format_error(), what));
		assert_equal(false, result.staged, "the DNG is not staged and swapped" + what);
		assert_equal(true, dng_before == platform::blob_from_file(path), "the DNG is untouched" + what);
		assert_equal(modified_before, platform::file_attributes(path).modified, "and keeps its time" + what);

		prop::item_metadata sidecar;
		metadata_xmp::parse(sidecar, path_xmp);
		assert_equal(4, sidecar.rating, "the rating is in the sidecar" + what);
		assert_equal("Sidecar title", sidecar.title, "beside what the sidecar held" + what);
		if (darktable_name) assert_equal(false, platform::exists(path.extension(".xmp")), "no second sidecar is made");

		// Converted, the DNG gives what it shows: its sidecar's.
		const auto converted = test_temps.next_path(".jpg");
		platform::copy_file(test_files_folder().combine_file("Test.jpg"), converted, false, false);
		assert_equal(true, metadata_xmp::update(converted, path, {}, path_xmp.name(), {}).success,
		             "a conversion is written" + what);
		assert_equal(4, own_packet_rating(converted), "and carries the sidecar's rating" + what);
	}
}

// A DNG with no sidecar keeps its metadata in itself, as Lightroom always writes one, and none is made.
static void should_write_a_dng_itself_without_a_sidecar()
{
	const auto path = test_temps.next_path(".dng");
	platform::copy_file(test_files_folder().combine_file("Small.tif"), path, false, false);

	metadata_edits edits;
	edits.rating = 3;
	files ff;
	const auto result = ff.update(path, edits, {}, {}, false, {});

	assert_equal(true, result.success(), std::format("rating saved ({})", result.format_error()));
	assert_equal(false, platform::exists(path.extension(".xmp")), "no sidecar is made");
	assert_equal(3, own_packet_rating(path), "the rating is in the DNG");
}

// A backup stages the DNG rather than writing the sidecar alone, and the stage's metadata still goes to
// the sidecar: the DNG put back is the one that was there.
static void should_write_a_dng_sidecar_when_backing_up()
{
	const auto path = test_temps.next_path(".dng");
	const auto path_xmp = path.extension(".xmp");
	platform::copy_file(test_files_folder().combine_file("Small.tif"), path, false, false);
	write_xmp_packet(path_xmp, "<xmp:Rating>1</xmp:Rating>");
	const auto dng_before = platform::blob_from_file(path);

	metadata_edits edits;
	edits.rating = 5;
	files ff;
	const auto result = ff.update(path, edits, {}, {}, true, path_xmp.name());

	assert_equal(true, result.success(), std::format("rating saved ({})", result.format_error()));
	assert_equal(true, dng_before == platform::blob_from_file(path), "the DNG holds what it did");

	prop::item_metadata sidecar;
	metadata_xmp::parse(sidecar, path_xmp);
	assert_equal(5, sidecar.rating, "the rating is in the sidecar");
}

// Where a DNG's metadata lives is decided once, before a sidecar-only edit. Asked again while another
// application removed the sidecar - darktable, or a sync client, between two file system queries - the
// update wrote the edit straight into the live DNG, unstaged, then reported a failure. Told the
// decision, it writes the stage the caller swaps in, from the DNG's own packet, and the DNG keeps every
// byte.
static void should_keep_a_dng_unchanged_when_its_sidecar_vanishes()
{
	const auto path = test_temps.next_path(".dng");
	platform::copy_file(test_files_folder().combine_file("Small.tif"), path, false, false);
	const auto dng_before = platform::blob_from_file(path);
	const auto staged = test_temps.next_path(".xmp");

	metadata_edits edits;
	edits.rating = 2;
	const auto result = metadata_xmp::update(path, path, edits, {}, staged, true);

	assert_equal(true, result.success, "the edit is written");
	assert_equal(true, dng_before == platform::blob_from_file(path), "the DNG holds what it did");
	assert_equal(true, result.xmp_path == staged, "to the staged sidecar");

	prop::item_metadata sidecar;
	metadata_xmp::parse(sidecar, staged);
	assert_equal(2, sidecar.rating, "the rating is in the sidecar");
}

static void should_preserve_orientation()
{
	const auto load_path = test_files_folder().combine_file("exif-rotated.jpg");

	files ff;
	const auto loaded = ff.load(load_path, false);
	const auto surface = loaded.to_surface();
	const auto loaded_orientation = loaded.i->orientation();

	const auto encoded_jpg = ff.surface_to_image(surface, {}, {}, core::image_format::JPEG);
	assert_equal(loaded_orientation, encoded_jpg->orientation(), "orientation for created jpg");

	const auto encoded_png = ff.surface_to_image(surface, {}, {}, core::image_format::PNG);
	assert_equal(loaded_orientation, encoded_png->orientation(), "orientation for created png");

	const auto encoded_webp = ff.surface_to_image(surface, {}, {}, core::image_format::WEBP);
	assert_equal(loaded_orientation, encoded_webp->orientation(), "orientation for created webp");

	const auto loaded_jpg = load_image_file(encoded_jpg->data());
	assert_equal(loaded_orientation, loaded_jpg->orientation(), "orientation for re-loaded jpg");

	const auto loaded_png = load_image_file(encoded_png->data());
	assert_equal(loaded_orientation, loaded_png->orientation(), "orientation for re-loaded png");

	const auto loaded_webp = load_image_file(encoded_webp->data());
	assert_equal(loaded_orientation, loaded_webp->orientation(), "orientation for re-loaded webp");
}

// A small (single APP2 segment) ICC profile must round-trip unchanged through a
// JPEG encode followed by a metadata scan.
static void should_roundtrip_small_icc()
{
	files ff;
	const auto load_path = test_files_folder().combine_file("Test.jpg");
	const auto loaded = ff.load(load_path, false);
	const auto surface = loaded.to_surface();

	libfiles::metadata_blocks metadata;
	metadata.icc.resize(3u * 1024u);
	for (size_t i = 0; i < metadata.icc.size(); ++i)
	{
		metadata.icc[i] = static_cast<uint8_t>((i * 17u + 3u) & 0xffu);
	}

	const auto encoded = ff.surface_to_image(surface, metadata, {}, core::image_format::JPEG);

	mem_read_stream stream(encoded->data());
	const auto scanned = scan_photo(stream);

	assert_equal(metadata.icc.size(), scanned.metadata.icc.size(), "small icc size round-trips");
	assert_equal(true, metadata.icc == scanned.metadata.icc, "small icc bytes round-trip");
}

// Regression: a large ICC profile exceeds the 65533-byte JPEG marker limit, so
// it must be split across multiple APP2 segments on write and reassembled in
// sequence order on read without any loss or reordering.
static void should_roundtrip_large_icc()
{
	files ff;
	const auto load_path = test_files_folder().combine_file("Test.jpg");
	const auto loaded = ff.load(load_path, false);
	const auto surface = loaded.to_surface();

	// 200 KB => 4 APP2 segments (each holds ~65519 profile bytes).
	libfiles::metadata_blocks metadata;
	metadata.icc.resize(200u * 1024u);
	for (size_t i = 0; i < metadata.icc.size(); ++i)
	{
		metadata.icc[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xffu);
	}

	const auto encoded = ff.surface_to_image(surface, metadata, {}, core::image_format::JPEG);

	mem_read_stream stream(encoded->data());
	const auto scanned = scan_photo(stream);

	assert_equal(metadata.icc.size(), scanned.metadata.icc.size(), "large icc size round-trips");
	assert_equal(true, metadata.icc == scanned.metadata.icc, "large icc bytes round-trip");
}

// An XMP packet carrying a long description, padded so its total size is exact and the JPEG
// marker boundary can be hit rather than approached.
static std::vector<uint8_t> make_xmp_packet(const size_t total_size)
{
	const std::string prefix =
		R"(<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?><x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:description><rdf:Alt><rdf:li xml:lang="x-default">)";
	const std::string suffix =
		R"(</rdf:li></rdf:Alt></dc:description></rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="w"?>)";

	auto packet = prefix;
	packet.append(total_size - prefix.size() - suffix.size(), 'D');
	packet += suffix;

	return {packet.begin(), packet.end()};
}

// The largest XMP that still fits one APP1 segment must round-trip, so rejecting oversized blocks
// cannot cost a description that was always writable.
static void should_roundtrip_largest_xmp()
{
	files ff;
	const auto load_path = test_files_folder().combine_file("Test.jpg");
	const auto loaded = ff.load(load_path, false);
	const auto surface = loaded.to_surface();

	libfiles::metadata_blocks metadata;
	metadata.xmp = make_xmp_packet(65533u - xmp_signature_len);

	const auto encoded = ff.surface_to_image(surface, metadata, {}, core::image_format::JPEG);

	mem_read_stream stream(encoded->data());
	const auto scanned = scan_photo(stream);

	assert_equal(metadata.xmp.size(), scanned.metadata.xmp.size(), "largest xmp size round-trips");
	assert_equal(true, metadata.xmp == scanned.metadata.xmp, "largest xmp bytes round-trip");
}

// Regression: a description that pushes XMP past the 65533-byte marker limit used to be dropped,
// writing an image stripped of its tags with nothing said about it. It must be reported instead,
// and the encoder must stay usable for the next file in the run.
static void should_report_oversized_xmp()
{
	files ff;
	const auto load_path = test_files_folder().combine_file("Test.jpg");
	const auto loaded = ff.load(load_path, false);
	const auto surface = loaded.to_surface();

	libfiles::metadata_blocks oversized;
	oversized.xmp = make_xmp_packet(65534u - xmp_signature_len);

	auto reported = false;

	try
	{
		ff.surface_to_image(surface, oversized, {}, core::image_format::JPEG);
	}
	catch (const std::exception&)
	{
		reported = true;
	}

	assert_equal(true, reported, "oversized xmp is reported rather than dropped");

	libfiles::metadata_blocks writable;
	writable.xmp = make_xmp_packet(1024u);

	const auto encoded = ff.surface_to_image(surface, writable, {}, core::image_format::JPEG);
	assert_equal(true, is_valid(encoded), "encoder still usable after a rejected file");

	mem_read_stream stream(encoded->data());
	const auto scanned = scan_photo(stream);
	assert_equal(true, writable.xmp == scanned.metadata.xmp, "next file keeps its xmp");
}

// Edit's lossless audio and video edits go through the staged write. FFmpeg's muxer writes back only the tags
// its demuxer read, so the copy is given the source's packet and the rating and tags the item shows, which the
// format keeps besides it - an MP4's Xtra box, an MP3's ID3 frames - and the saved file still says what it did.
// A backup keeps the original bytes, and a cancelled save leaves the file as it was.
static void should_keep_metadata_through_a_lossless_edit(const std::string_view name, const av_edits& edits)
{
	const auto source = test_files_folder().combine_file(name);
	const auto path = test_temps.next_path(source.extension());
	platform::copy_file(source, path, false, false);

	files ff;
	metadata_edits tagged;
	tagged.rating = 4;
	tagged.add_tags = tag_set("trimmed holiday");
	assert_equal(true, ff.update(path, tagged, {}, {}, false, {}).success(), std::format("{} is rated", name));
	const auto before = ff_scan_file(ff, path).to_props();
	const auto bytes_before = read_test_file(path);

	// As Edit carries them, from what the item shows.
	metadata_edits carried;
	carried.rating = before->rating;
	carried.add_tags = tag_set(before->tags.sv());

	std::atomic_bool cancelled = true;
	const auto refused = ff.update_av(path, path, edits, carried, true, {}, {}, {}, core::cancel_token(cancelled));
	assert_equal(true, refused.code == platform::file_op_result_code::CANCELLED,
	             std::format("{}: a cancelled save says so", name));
	assert_equal(true, read_test_file(path) == bytes_before, std::format("{}: and leaves the file as it was", name));

	const core::cancel_token never;
	const auto saved = ff.update_av(path, path, edits, carried, true, {}, {}, ff_inspect_rescan(path), never);
	assert_equal(true, saved.success(), std::format("{}: the edit is saved: {}", name, saved.error_message));
	assert_equal(true, saved.staged, std::format("{}: staged and swapped in", name));

	const auto after = ff_scan_file(ff, path).to_props();
	assert_equal(4, after->rating, std::format("{}: the rating survives", name));
	assert_equal(before->tags.sv(), after->tags.sv(), std::format("{}: the tags survive", name));
	if (edits.remove_audio) assert_equal(true, after->audio_codec.is_empty(), std::format("{}: no sound", name));

	const auto original_path = core::file_path(path.folder(),
	                                           std::string(path.file_name_without_extension()) + ".original",
	                                           path.extension());
	assert_equal(true, read_test_file(original_path) == bytes_before,
	             std::format("{}: the backup is the original", name));
	platform::delete_file(original_path);
}

void register_media_edit_tests(view_state& /*state*/, test_registry& tests)
{
	//
	// Lossless audio and video edits
	//
	tests.add("Should keep metadata through a lossless edit gizmo.mp4"s, []
	{
		av_edits edits;
		edits.remove_audio = true;
		should_keep_metadata_through_a_lossless_edit("gizmo.mp4", edits);
	});
	tests.add("Should keep metadata through a lossless edit ipod.mov"s, []
	{
		av_edits edits;
		edits.quarter_turns = 1;
		should_keep_metadata_through_a_lossless_edit("ipod.mov", edits);
	});
	tests.add("Should keep metadata through a lossless edit Colorblind.mp3"s, []
	{
		av_edits edits;
		edits.start = 1.0;
		edits.end = 3.0;
		should_keep_metadata_through_a_lossless_edit("Colorblind.mp3", edits);
	});

	//
	// Modify media
	//
	constexpr std::string_view common_files[] = {
		"Byzantium.avi", "cherrys.psd", "Colorblind.mp3",
		"engine.png",    "gen.asf",     "gen.flv",
		"gen.mpg",       "gen.wav",     "gen.wmv",
		"Gherkin.CR2",   "gizmo.mp4",   "IMG_0096.JPG",
		"cmyk.jpg",      "ipod.mov",    "jello.tif",
		"StPauls.MOV",   "Test.jpg",    "tuesday.gif",
		"lake.webp",     "melnik.heic", "hato.profile0.10bpc.yuv420.avif",
	};

	for (auto name : common_files)
	{
		tests.add(std::format("Should update metadata {}", name), [name] { should_update_metadata(name); });
		tests.add(std::format("Should update location {}", name), [name] { should_update_location(name); });
		tests.add(std::format("Should update rating {}", name), [name] { should_update_rating(name); });
		tests.add(std::format("Should update label {}", name), [name] { should_update_label(name); });
		tests.add(std::format("Should update tags {}", name), [name] { should_add_remove_tags(name); });
	}

	tests.add("Should update gps in exif"s, should_update_gps_in_exif);
	for (const auto name :
	     {"engine.png"sv, "tuesday.gif"sv, "lake.webp"sv, "Test.jpg"sv, "jello.tif"sv, "cherrys.psd"sv, "gizmo.mp4"sv,
	      "ipod.mov"sv, "StPauls.MOV"sv, "gen.asf"sv, "gen.wmv"sv, "Byzantium.avi"sv, "gen.wav"sv, "Colorblind.mp3"sv,
	      "gen.flv"sv, "melnik.heic"sv, "hato.profile0.10bpc.yuv420.avif"sv})
	{
		tests.add(std::format("Should write packets with libfiles {}", name),
		          [name] { should_write_packets_with_libfiles(name); });
	}
	tests.add("Should round-trip Xmp disc metadata Byzantium.avi"s, should_roundtrip_xmp_disc_metadata);
	tests.add("Should carry shown metadata into a converted file"s, should_carry_shown_metadata_into_a_converted_file);
	tests.add("Should carry shown metadata into a JPEG XL"s, should_carry_shown_metadata_into_a_jxl);
	tests.add("Should repack a JPEG as a lossless JPEG XL that gives it back"s,
	          should_repack_a_jpeg_as_a_lossless_jxl_that_gives_it_back);
	tests.add("Should turn a JPEG XL repacked from a JPEG as the JPEG would turn"s,
	          should_turn_a_jxl_repacked_from_a_jpeg_as_the_jpeg_would_turn);
	tests.add("Should rate and tag a JPEG XL"s, should_rate_and_tag_a_jxl);
	tests.add("Should rate a bare JPEG XL"s, should_rate_a_bare_jxl);
	tests.add("Should rate and tag a HEIC"s, [] { should_rate_and_tag_a_heif("melnik.heic"); });
	tests.add("Should rate and tag an AVIF"s, [] { should_rate_and_tag_a_heif("hato.profile0.10bpc.yuv420.avif"); });
	for (const auto ext : {".jpx"sv, ".wbmp"sv})
	{
		tests.add(std::format("Should write a sidecar for an image without a writer {}", ext),
		          [ext] { should_write_a_sidecar_for_an_image_without_a_writer(ext); });
	}
	tests.add("Should refuse an edit to FLAC behind an ID3 tag"s, should_refuse_an_edit_to_flac_behind_an_id3_tag);
	tests.add("Should preserve metadata creator names with apostrophes"s,
	          should_preserve_creator_names_with_apostrophes);
	// Issue #252 - rotated video keeps its rotation across a metadata write
	tests.add("Should keep video rotation through a metadata write"s,
	          should_keep_video_rotation_through_a_metadata_write);
	// Issue #134 - emoji filenames
	tests.add("Should update rating and label for emoji filename"s, should_update_rating_and_label_for_emoji_filename);
	tests.add("Should handle international characters"s, should_handle_international_characters);
	tests.add("Should handle korean characters"s, should_handle_korean_characters);
	tests.add("Should edit video metadata in place gizmo.mp4"s,
	          [] { should_edit_video_metadata_in_place("gizmo.mp4"); });
	tests.add("Should edit video metadata in place ipod.mov"s, [] { should_edit_video_metadata_in_place("ipod.mov"); });
	tests.add("Should edit video metadata in place gen.asf"s, [] { should_edit_video_metadata_in_place("gen.asf"); });
	tests.add("Should inject video metadata in place indy.mp4"s,
	          [] { should_inject_video_metadata_in_place("indy.mp4"); });
	// Faststart: moov leads the file with only an 8-byte free box after it, so a first packet
	// cannot grow in place and the handler has to relocate rather than fall back to a copy.
	tests.add("Should inject video metadata in place anamorphic.mp4"s,
	          [] { should_inject_video_metadata_in_place("anamorphic.mp4"); });
	tests.add("Should patch a metadata edit that fits Test.jpg"s,
	          [] { should_patch_a_metadata_edit_that_fits("Test.jpg"); });
	tests.add("Should patch a metadata edit that fits engine.png"s,
	          [] { should_patch_a_metadata_edit_that_fits("engine.png"); });
	tests.add("Should patch a metadata edit that fits tuesday.gif"s,
	          [] { should_patch_a_metadata_edit_that_fits("tuesday.gif"); });
	tests.add("Should patch a metadata edit that fits lake.webp"s,
	          [] { should_patch_a_metadata_edit_that_fits("lake.webp"); });
	tests.add("Should patch a metadata edit that fits Small.tif"s,
	          [] { should_patch_a_metadata_edit_that_fits("Small.tif"); });
	tests.add("Should patch a metadata edit that fits cherrys.psd"s,
	          [] { should_patch_a_metadata_edit_that_fits("cherrys.psd"); });
	tests.add("Should patch a metadata edit that fits Colorblind.mp3"s,
	          [] { should_patch_a_metadata_edit_that_fits("Colorblind.mp3"); });
	tests.add("Should patch a metadata edit that fits gen.wav"s,
	          [] { should_patch_a_metadata_edit_that_fits("gen.wav"); });
	tests.add("Should patch a metadata edit that fits Byzantium.avi"s,
	          [] { should_patch_a_metadata_edit_that_fits("Byzantium.avi"); });
	tests.add("Should patch a metadata edit that fits gen.flv"s,
	          [] { should_patch_a_metadata_edit_that_fits("gen.flv"); });
	tests.add("Should stage a metadata edit that does not fit"s, should_stage_a_metadata_edit_that_does_not_fit);
	tests.add("Should stage an edit as copying and updating would"s,
	          should_stage_an_edit_as_copying_and_updating_would);
	tests.add("Should edit raw sidecar only"s, should_edit_raw_sidecar_only);
	tests.add("Should write raw sidecar original date"s, should_write_raw_sidecar_original_date);
	// SRC-020 - first RAW sidecar starts from effective embedded XMP
	tests.add("Should seed first raw sidecar from embedded xmp"s, should_seed_first_raw_sidecar_from_embedded_xmp);
	// SRC-020 - existing sidecar remains the source packet
	tests.add("Should use existing partial raw sidecar as source"s, should_use_existing_partial_raw_sidecar_as_source);
	// SRC-020 - explicit empty dc:subject has precedence over embedded tags
	tests.add("Should keep empty raw sidecar subject authoritative"s,
	          should_keep_empty_raw_sidecar_subject_authoritative);
	// SRC-030 - failed in-place writes that may have touched live bytes force recovery scans
	tests.add("Should force rescan after failed live metadata write"s,
	          should_force_rescan_after_failed_live_metadata_write);
	// SRC-030 - date batches need the same recovery folder validation as metadata batches
	tests.add("Should revalidate folder after failed live date write"s,
	          should_revalidate_folder_after_failed_live_date_write);
	// SRC-030 - a retry that mutates and throws must still force a rescan
	tests.add("Should mark retry failure after live metadata mutation"s,
	          should_mark_retry_failure_after_live_metadata_mutation);
#ifdef _WIN32
	// SRC-020 - unknown RAW source metadata must not be replaced by a default packet
	tests.add("Should refuse first raw sidecar when raw source is unreadable"s,
	          should_refuse_first_raw_sidecar_when_raw_source_is_unreadable);
#endif
	tests.add("Should refuse to write an unreadable sidecar"s, should_refuse_to_write_an_unreadable_sidecar);
	tests.add("Should save as with distinct xmp sidecar"s, should_save_as_with_distinct_xmp_sidecar);
	tests.add("Should write a DNG sidecar when it has one"s, should_write_a_dng_sidecar_when_it_has_one);
	tests.add("Should write a DNG itself without a sidecar"s, should_write_a_dng_itself_without_a_sidecar);
	tests.add("Should write a DNG sidecar when backing up"s, should_write_a_dng_sidecar_when_backing_up);
	tests.add("Should keep a DNG unchanged when its sidecar vanishes"s,
	          should_keep_a_dng_unchanged_when_its_sidecar_vanishes);
	tests.add("Should update exif rating"s, should_update_exif_rating);
	tests.add("Should write an edited original date"s, should_write_an_edited_original_date);
	tests.add("Should write an edited created date"s, should_write_an_edited_created_date);
	tests.add("Should update formatted description"s, should_update_formatted_text);
	tests.add("Should update synopsis"s, should_update_synopsis);

	// Windows Explorer / Media Player tag interop (#123)
	tests.add("Should read mp4 Xtra metadata gizmo.mp4"s, [] { should_read_mp4_xtra_metadata("gizmo.mp4", "xTag1"); });
	tests.add("Should read mp4 Xtra metadata ipod.mov"s, [] { should_read_mp4_xtra_metadata("ipod.mov", "ipad"); });

#ifdef _WIN32
	// These write through the Windows property system and then ask it to read the result back, so
	// they measure Diffractor against the shell itself. There is nothing to hold them to elsewhere:
	// platform::write_shell_tags has no counterpart, and a test that skips asserts nothing and fails.
	tests.add("Should remove shell written tags"s, should_remove_shell_written_tags);
	tests.add("Should read asf WM/Category multi-value"s, should_read_asf_wm_categories);
	tests.add("Should write windows tags via xmp gizmo.mp4"s, [] { should_write_windows_tags_via_xmp("gizmo.mp4"); });
	tests.add("Should write windows tags via xmp ipod.mov"s, [] { should_write_windows_tags_via_xmp("ipod.mov"); });
	tests.add("Should write windows tags via xmp gen.wmv"s, [] { should_write_windows_tags_via_xmp("gen.wmv"); });

	constexpr std::string_view shell_tag_files[] = {"gizmo.mp4", "ipod.mov", "gen.wmv", "gen.asf"};
	for (auto name : shell_tag_files)
	{
		tests.add(std::format("Should round-trip windows shell tags {}", name),
		          [name] { should_roundtrip_windows_shell_tags(name); });
	}
#endif

	// Unlike the above, this one asserts that the write is refused, which is true everywhere.
	constexpr std::string_view shell_unsupported_files[] = {"Byzantium.avi", "gen.mpg"};
	for (auto name : shell_unsupported_files)
	{
		tests.add(std::format("Should handle unsupported shell tags {}", name),
		          [name] { should_handle_unsupported_shell_tags(name); });
	}

	//
	// Bitmap Edit
	//
	tests.add("Should preserve orientation"s, should_preserve_orientation);
	tests.add("Should apply perspective correction"s, should_apply_perspective_correction);
	tests.add("Should fit document correction to controls"s, should_fit_document_correction_to_controls);
	tests.add("Should detect only clear document regions"s, should_detect_only_clear_document_regions);
	tests.add("Should detect photographed document"s, should_detect_photographed_document);
	tests.add("Should detect document at image edge"s, should_detect_document_at_image_edge);
	tests.add("Should detect low contrast document"s, should_detect_low_contrast_document);
	tests.add("Should apply temperature and tint"s, should_apply_temperature_and_tint);
	tests.add("Should auto colour against a cast"s, should_auto_colour_against_a_cast);
	tests.add("Should crop without resampling"s, should_crop_without_resampling);
	tests.add("Should initialize edit rotation from orientation"s, should_initialize_edit_rotation_from_orientation);
	tests.add("Should rotate displayed oriented pixels clockwise"s, should_rotate_displayed_oriented_pixels_clockwise);
	tests.add("Should build rotated edit preview surface"s, should_build_rotated_edit_preview_surface);
	tests.add("Should build straightened edit preview without black corners"s,
	          should_build_straightened_edit_preview_without_black_corners);
	tests.add("Should not treat initial orientation as an edit"s, should_not_treat_initial_orientation_as_an_edit);
	tests.add("Should save oriented edit upright"s, should_save_oriented_edit_upright);
	tests.add("Should keep a gain map through an edit"s, should_keep_a_gain_map_through_an_edit);
	tests.add("Should keep sixteen bits through an edit"s, should_keep_sixteen_bits_through_an_edit);
	tests.add("Should keep a photo's bits through an edit saved over it"s,
	          should_keep_a_photos_bits_through_an_edit_saved_over_it);
	tests.add("Should keep HDR through an edit"s, should_keep_hdr_through_an_edit);
	tests.add("Should keep an AVIF gain map through an edit"s, should_keep_an_avif_gain_map_through_an_edit);
	tests.add("Should not write a narrowed HDR photo back over itself"s,
	          should_not_write_a_narrowed_hdr_photo_back_over_itself);
	tests.add("Should show and edit a wide-gamut photo as sRGB"s, should_show_and_edit_a_wide_gamut_photo_as_srgb);
	tests.add("Should develop a raw at sixteen bits for an edit"s, should_develop_a_raw_at_sixteen_bits_for_an_edit);
	tests.add("Should round-trip small ICC"s, should_roundtrip_small_icc);
	tests.add("Should round-trip large ICC"s, should_roundtrip_large_icc);
	tests.add("Should round-trip largest XMP"s, should_roundtrip_largest_xmp);
	tests.add("Should report oversized XMP"s, should_report_oversized_xmp);
}
