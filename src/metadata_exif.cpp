// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: EXIF metadata, which libfiles reads, writes and lists: what an item takes from a block; the
// verbose pane's listing, which presents the maker note as its own directory and derives the facts the
// camera implied but did not record; and the EXIF written with a re-encoded or turned image.

#include "pch.h"
#include "metadata_exif.h"
#include "files.h"
#include "model_location.h"

#include "libfiles/exif.h"
#include "libfiles/exif_listing.h"
#include "libfiles/scan.h"

#include <utility>

df_assert_pod(metadata_exif::urational32_t);
df_assert_pod(metadata_exif::srational32_t);

static bool is_junk(const uint8_t* p, const uint32_t s)
{
	static constexpr uint8_t junk_marker[] = {0x12, 0xf8, 0x0f, 0x3b,};
	return s >= 4 && memcmp(p, junk_marker, 4) == 0;
}

// Vendors (e.g. Samsung SM-G900F) pack binary blobs into text tags such as UserComment,
// sometimes behind a valid "ASCII\0\0\0" character code. Control characters other than
// tab, newline and return prove the payload is not readable text.
static bool is_binary_text(const std::string_view text)
{
	for (const auto c : text)
	{
		const auto u = static_cast<uint8_t>(c);
		if (u < 0x20 && u != '\t' && u != '\n' && u != '\r') return true;
		if (u == 0x7f) return true;
	}

	return false;
}

// Samsung ISP writes a "JKJK" scene/exposure block map plus "FAFA" focus records into text tags.
// Naming the block is more use in the verbose listing than an empty or truncated value.
static std::string_view vendor_blob_name(const uint8_t* p, const uint32_t len)
{
	if (!p) return {};
	const std::string_view sv{std::bit_cast<const char*>(p), len};
	if (sv.find("JKJK") != std::string_view::npos) return "Samsung camera debug data";
	return {};
}

// A date, its zone and its fraction are three tags that may come in any order, so libfiles keeps
// them as text and they are combined here, once.
static void add_exif_date(prop::item_metadata& md, const libfiles::exif_date& date, const prop::date_source source)
{
	if (date.text.empty()) return;

	df::date_t d;
	if (!d.parse_exif_date(date.text) || !d.is_valid()) return;

	if (const auto fraction = prop::parse_sub_second_intervals(date.sub_second); fraction != 0)
	{
		d = df::date_t(d.to_int64() + fraction);
	}

	md.dates.add(source, d, prop::parse_utc_offset(date.offset));
}

// GPSDateStamp and GPSTimeStamp are a true UTC instant for the moment of capture, which is the one
// thing that can recover a zone an EXIF reading never stated. Stored, never displayed.
static void add_gps_stamp(prop::item_metadata& md, const libfiles::exif_metadata& m)
{
	if (m.gps_date.empty() || !m.gps_time) return;

	df::date_t d;
	// `CCYY:MM:DD` with no time: the shared parser has no date-only form for the colon spelling, so
	// the pair is punctuated here, exactly as add_iptc_date does.
	if (!d.parse_exif_date(m.gps_date + " 00:00:00") || !d.is_valid()) return;

	const auto& time = *m.gps_time;
	const auto seconds = static_cast<uint64_t>(time[0]) * 3600 + static_cast<uint64_t>(time[1]) * 60 +
		static_cast<uint64_t>(time[2]);

	if (seconds >= 24 * 3600) return;

	md.dates.add_utc(prop::date_source::gps_stamp,
	                 df::date_t(d.to_int64() + seconds * df::date_t::intervals_per_second));
}

static void assign_text(str::cached& field, const std::string& value)
{
	if (!value.empty()) field = str::cache(value);
}

ui::orientation metadata_exif::orientation(const df::cspan cs)
{
	const auto stored = libfiles::summarize_exif({cs.data, cs.size}).orientation;
	return stored ? static_cast<ui::orientation>(*stored) : ui::orientation::top_left;
}

void metadata_exif::parse(prop::item_metadata& pd, const df::cspan cs)
{
	if (cs.empty()) return;

	const auto m = libfiles::read_exif({cs.data, cs.size});

	if (m.orientation) pd.orientation = static_cast<ui::orientation>(*m.orientation);

	assign_text(pd.camera_manufacturer, m.camera_make);
	assign_text(pd.camera_model, m.camera_model);
	assign_text(pd.lens, m.lens);
	assign_text(pd.encoder, m.software);
	assign_text(pd.artist, m.artist);
	assign_text(pd.copyright_notice, m.copyright);
	assign_text(pd.description, m.description);
	assign_text(pd.comment, m.comment);
	assign_text(pd.title, m.title);
	assign_text(pd.tags, m.keywords);

	// Every caller reads EXIF into a fresh item, before IPTC and XMP, so what EXIF does not record is
	// already zero.
	pd.f_number = m.f_number;
	pd.exposure_time = m.exposure_time;
	pd.focal_length = m.focal_length;
	pd.focal_length_35mm_equivalent = m.focal_length_35mm;
	pd.iso_speed = m.iso_speed;
	if (m.rating) pd.rating = *m.rating;

	if (m.latitude && m.longitude)
	{
		const gps_coordinate coordinate(*m.latitude, *m.longitude);
		pd.coordinate = coordinate.is_valid() ? coordinate : gps_coordinate{};
	}

	// Set as EXIF reads them, below sea level and without a speed included.
	pd.altitude = m.altitude;
	pd.gps_speed = m.speed;

	add_exif_date(pd, m.original, prop::date_source::exif_original);
	add_exif_date(pd, m.digitized, prop::date_source::exif_digitized);
	add_exif_date(pd, m.modified, prop::date_source::exif_datetime);
	add_gps_stamp(pd, m);
}

// A lossless turn changes the pixels' orientation and dimensions and nothing else, so the block is
// patched where those numbers stand: upright in IFD0 and IFD1, the turned size in the Exif and
// Interoperability directories. Nothing else in it changes or moves, so a maker note keeps the
// absolute offsets it relies on. libexif rewrote the whole block, which emptied five of the fixtures'
// Canon maker notes and re-encoded five others.
df::blob metadata_exif::fix_dims(const df::span cs, const int image_width, const int image_height)
{
	df::assert_true(is_exif_signature(cs));

	df::blob result(cs.data, cs.data + cs.size);
	if (result.size() <= exif_signature.size()) return result;

	const auto width = static_cast<uint32_t>(image_width);
	const auto height = static_cast<uint32_t>(image_height);

	using libfiles::exif_directory;
	const libfiles::exif_number numbers[] = {
		{exif_directory::primary, 0x0112, 1}, // Orientation
		{exif_directory::thumbnail, 0x0112, 1},
		{exif_directory::exif, 0xa002, width}, // PixelXDimension
		{exif_directory::exif, 0xa003, height}, // PixelYDimension
		{exif_directory::interoperability, 0x1001, width}, // RelatedImageWidth
		{exif_directory::interoperability, 0x1002, height}, // RelatedImageLength
	};

	libfiles::set_exif_numbers({result.data() + exif_signature.size(), result.size() - exif_signature.size()}, numbers);
	return result;
}

static std::string exif_date_text(const df::date_t date)
{
	const auto d = date.date();
	return std::format("{:04}:{:02}:{:02} {:02}:{:02}:{:02}", d.year, d.month, d.day, d.hour, d.minute, d.second);
}

df::blob metadata_exif::make_exif(const prop::item_metadata_ptr& md)
{
	libfiles::exif_fields f;

	if (md->orientation != ui::orientation::none) f.orientation = static_cast<uint16_t>(md->orientation);
	if (!prop::is_null(md->f_number)) f.f_number = md->f_number;
	if (!prop::is_null(md->exposure_time)) f.exposure_time = md->exposure_time;
	if (!prop::is_null(md->iso_speed)) f.iso_speed = md->iso_speed;
	if (!prop::is_null(md->focal_length)) f.focal_length = md->focal_length;
	if (!prop::is_null(md->focal_length_35mm_equivalent)) f.focal_length_35mm = md->focal_length_35mm_equivalent;
	if (!prop::is_null(md->camera_manufacturer)) f.camera_make = md->camera_manufacturer.sv();
	if (!prop::is_null(md->camera_model)) f.camera_model = md->camera_model.sv();
	if (!prop::is_null(md->artist)) f.artist = md->artist.sv();
	if (!prop::is_null(md->lens)) f.lens = md->lens.sv();
	if (!prop::is_null(md->copyright_notice)) f.copyright = md->copyright_notice.sv();
	if (!prop::is_null(md->description)) f.description = md->description.sv();
	if (!prop::is_null(md->comment)) f.comment = md->comment.sv();

	// Never the resolved date: writing that into DateTimeOriginal would forge a capture time for any
	// file whose Original was inferred from a lower-authority source (#184, #192).
	if (const auto original = md->dates.original(); original.is_valid()) f.original = exif_date_text(original);
	if (const auto modified = md->dates.modified(); modified.is_valid()) f.modified = exif_date_text(modified);
	if (const auto created = md->dates.created(); created.is_valid()) f.digitized = exif_date_text(created);

	const auto tiff = libfiles::make_exif(f);
	df::blob result(exif_signature.begin(), exif_signature.end());
	result.insert(result.end(), tiff.begin(), tiff.end());
	return result;
}

static std::string_view directory_title(const int ifd)
{
	switch (ifd)
	{
	case 0: return "Main image (IFD0)";
	case 1: return "Thumbnail (IFD1)";
	case 2: return "Exif";
	case 3: return "GPS";
	case 4: return "Interoperability";
	default: return "Other";
	}
}

// One numeric tag from the Exif directory: the same quantity is a rational in one tag and a short in
// another, so the caller asks for a number and not for a storage format.
static bool read_listed_number(const libfiles::exif_listing& listing, const uint16_t tag, double& out)
{
	for (const auto& directory : listing.directories)
	{
		if (directory.directory != libfiles::exif_directory::exif) continue;

		for (const auto& e : directory.entries)
		{
			if (e.tag != tag) continue;
			if (e.bytes.empty() || e.count == 0) return false;

			const auto* const p = e.bytes.data();
			const auto little = listing.little_endian;
			const auto u16 = [&](const uint8_t* q) { return static_cast<uint16_t>(little ? q[0] | q[1] << 8 : q[0] << 8 | q[1]); };
			const auto u32 = [&](const uint8_t* q)
			{
				return little
					       ? static_cast<uint32_t>(q[0]) | static_cast<uint32_t>(q[1]) << 8 | static_cast<uint32_t>(q[2]) << 16 |
					       static_cast<uint32_t>(q[3]) << 24
					       : static_cast<uint32_t>(q[0]) << 24 | static_cast<uint32_t>(q[1]) << 16 |
					       static_cast<uint32_t>(q[2]) << 8 | static_cast<uint32_t>(q[3]);
			};

			size_t size = 0;
			switch (e.type)
			{
			case 3: case 8: size = 2;
				break;
			case 4: case 9: size = 4;
				break;
			case 5: case 10: size = 8;
				break;
			default: return false;
			}

			if (e.bytes.size() < size) return false;

			switch (e.type)
			{
			case 3: out = u16(p);
				return true;
			case 8: out = static_cast<int16_t>(u16(p));
				return true;
			case 4: out = u32(p);
				return true;
			case 9: out = static_cast<int32_t>(u32(p));
				return true;
			case 5:
				if (u32(p + 4) == 0) return false;
				out = static_cast<double>(u32(p)) / u32(p + 4);
				return true;
			default:
				if (static_cast<int32_t>(u32(p + 4)) == 0) return false;
				out = static_cast<double>(static_cast<int32_t>(u32(p))) / static_cast<int32_t>(u32(p + 4));
				return true;
			}
		}

		return false;
	}

	return false;
}

metadata_kv_list metadata_exif::to_info(const df::cspan data)
{
	metadata_kv_list result;
	const auto listing = libfiles::list_exif({data.data, data.size});

	// Exif is a set of image file directories, and which directory a tag lives in is part of what the
	// block contains, so each is listed as its own section rather than flattened together.
	for (const auto& directory : listing.directories)
	{
		const auto ifd_index = static_cast<int>(directory.directory);
		auto& section = result.emplace_back(std::format("{} ({})", directory_title(ifd_index), directory.entries.size()),
		                                    std::string{});
		section.container = true;
		section.id = std::format("exif.ifd.{}", ifd_index);
		// The Exif directory holds the capture settings most readers came for, so it opens however many
		// tags the camera wrote.
		section.open_by_default = directory.directory == libfiles::exif_directory::exif;

		for (const auto& e : directory.entries)
		{
			const std::string_view rendered = e.value;
			const auto len = rendered.size();
			const auto vendor = vendor_blob_name(e.bytes.data(), static_cast<uint32_t>(e.bytes.size()));
			// An empty render over a payload of real size means the value stopped at a nul inside a binary
			// blob, so report the size rather than a blank value.
			const auto readable = vendor.empty() && (!rendered.empty() || e.bytes.size() <= 4) &&
				!is_junk(std::bit_cast<const uint8_t*>(rendered.data()), static_cast<uint32_t>(len)) &&
				!is_binary_text(rendered);

			auto& row = result.emplace_back(
				e.name ? *e.name : std::format("Tag 0x{:04x}", e.tag),
				readable
					? std::string(str::utf8_cast(rendered))
					: std::format("{}, {} bytes", vendor.empty() ? std::string_view{"binary"} : vendor, e.bytes.size()));

			row.depth = 1;
			row.shape = std::format("{} x{}, 0x{:04x}", libfiles::exif_type_name(e.type), e.count, e.tag);
			row.id = std::format("exif.{}.{:04x}", ifd_index, e.tag);

			// The bytes stay reachable whether or not they render; the hex listing is built only when the
			// row is opened.
			if (!e.bytes.empty())
			{
				const auto kept = std::min<size_t>(e.bytes.size(), str::max_hex_dump_bytes);
				row.detail = metadata_binary_detail{std::vector<uint8_t>(e.bytes.begin(), e.bytes.begin() + kept)};
			}
		}
	}

	// The maker note is a vendor's own directory carried inside Exif, so it is listed as its own section
	// rather than left as the binary blob the Exif entry reports. libfiles decodes the makes libexif did -
	// Canon, Nikon, Fuji, Olympus, Sanyo, Epson, Pentax and Casio - and anything else keeps only that row.
	if (!listing.maker_note.empty())
	{
		std::string title = "MakerNote";

		// The decoder does not say which vendor it recognised, so the camera's own Make names the section.
		for (const auto& directory : listing.directories)
		{
			const auto make = std::ranges::find(directory.entries, uint16_t{0x010f}, &libfiles::exif_listing_entry::tag);
			if (make == directory.entries.end()) continue;

			const auto trimmed = str::trim(std::string_view{make->value});
			if (!trimmed.empty() && !is_binary_text(trimmed)) title = std::format("MakerNote ({})", str::utf8_cast(trimmed));
			break;
		}

		auto& section = result.emplace_back(std::format("{} ({})", title, listing.maker_note.size()), std::string{});
		section.container = true;
		section.id = "exif.mnote";

		for (const auto& v : listing.maker_note)
		{
			const std::string_view rendered = v.value;
			const auto readable = !rendered.empty() &&
				!is_junk(std::bit_cast<const uint8_t*>(rendered.data()), static_cast<uint32_t>(rendered.size())) &&
				!is_binary_text(rendered);

			auto& row = result.emplace_back(v.name ? *v.name : std::format("Tag 0x{:04x}", v.id),
			                                readable ? std::string(str::utf8_cast(rendered)) : std::string{"binary"});

			row.depth = 1;
			row.shape = std::format("0x{:04x}", v.id);
			row.id = std::format("exif.mnote.{:04x}", v.id);
		}
	}

	// Facts the camera implied but did not record: arithmetic over the tags listed above.
	{
		metadata_kv_list rows;

		double focal_length = 0.0;
		double focal_35 = 0.0;
		double f_number = 0.0;
		double exposure = 0.0;
		double pixel_x = 0.0;
		double pixel_y = 0.0;

		const auto has_focal = read_listed_number(listing, 0x920a, focal_length) && focal_length > 0.0;
		const auto has_focal_35 = read_listed_number(listing, 0xa405, focal_35) && focal_35 > 0.0;
		const auto has_f_number = read_listed_number(listing, 0x829d, f_number) && f_number > 0.0;
		const auto has_exposure = read_listed_number(listing, 0x829a, exposure) && exposure > 0.0;
		const auto has_pixels = read_listed_number(listing, 0xa002, pixel_x) &&
			read_listed_number(listing, 0xa003, pixel_y) && pixel_x > 0.0 && pixel_y > 0.0;

		const auto crop = (has_focal && has_focal_35) ? focal_35 / focal_length : 0.0;

		if (crop > 0.0)
		{
			auto& row = rows.emplace_back("Crop factor"s, std::format("{:.2f}x", crop));
			row.shape = "focal.35 / focal";
		}

		if (has_focal_35)
		{
			const auto fov = to_degrees(2.0 * std::atan(36.0 / (2.0 * focal_35)));
			auto& row = rows.emplace_back("Field of view"s, std::format("{:.1f}\u00b0 horizontal", fov));
			row.shape = "36mm / focal.35";
		}

		const auto coc = crop > 0.0 ? 0.03 / crop : 0.0;

		if (coc > 0.0)
		{
			auto& row = rows.emplace_back("Circle of confusion"s, std::format("{:.3f} mm", coc));
			row.shape = "0.03mm / crop";
		}

		if (coc > 0.0 && has_f_number)
		{
			const auto hyperfocal = (focal_length * focal_length) / (f_number * coc) + focal_length;
			auto& row = rows.emplace_back("Hyperfocal distance"s, std::format("{:.2f} m", hyperfocal / 1000.0));
			row.shape = "focal2 / (fnumber x coc)";
		}

		if (has_f_number && has_exposure)
		{
			const auto light_value = 2.0 * std::log2(f_number) - std::log2(exposure);
			auto& row = rows.emplace_back("Light value"s, std::format("{:.1f} EV", light_value));
			row.shape = "aperture + shutter, ISO 100";
		}

		if (has_pixels)
		{
			const auto megapixels = (pixel_x * pixel_y) / 1'000'000.0;
			auto& row = rows.emplace_back("Megapixels"s, std::format("{:.1f} MP", megapixels));
			row.shape = "width x height";
		}

		if (!rows.empty())
		{
			auto& section = result.emplace_back(std::format("Derived ({})", rows.size()), std::string{});
			section.container = true;
			section.id = "exif.derived";

			auto index = 0;

			for (auto& row : rows)
			{
				row.depth = 1;
				row.id = std::format("exif.derived.{}", index++);
				result.emplace_back(std::move(row));
			}
		}
	}

	if (!listing.thumbnail.empty())
	{
		auto& row = result.emplace_back("Embedded thumbnail"_c, std::format("{} bytes", listing.thumbnail.size()));
		row.id = "exif.thumbnail";
		row.shape = "binary"_c;
		// The whole payload is kept rather than a hex-sized prefix: a truncated image cannot decode.
		constexpr size_t max_embedded_image_bytes = 8_z * 1024_z * 1024_z;
		const auto kept = std::min<size_t>(listing.thumbnail.size(), max_embedded_image_bytes);
		row.detail = metadata_binary_detail{
			std::vector<uint8_t>(listing.thumbnail.begin(), listing.thumbnail.begin() + kept), true
		};
	}

	return result;
}
