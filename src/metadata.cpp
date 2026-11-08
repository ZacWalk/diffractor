// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The metadata blocks a file carries - EXIF, IPTC, XMP and ICC profiles - which libfiles reads,
// writes and lists: the EXIF written with a re-encoded or turned image, the edits Diffractor makes
// written into XMP packets in files and sidecars, and each block listed for the verbose pane. What an
// item takes from a block is libfiles' one table, which apply_properties maps onto the item.

#include "pch.h"

#include "metadata.h"
#include "files.h"
#include "model_location.h"
#include "model_property.h"

#include "libfiles/exif.h"
#include "libfiles/exif_listing.h"
#include "libfiles/icc.h"
#include "libfiles/identify.h"
#include "libfiles/iptc.h"
#include "libfiles/scan.h"
#include "libfiles/write.h"
#include "libfiles/xmp.h"

#include <utility>

///////////////////////////////////////////////////////////////////////////////////////////////////
// EXIF metadata, which libfiles reads, writes and lists: the verbose pane's listing, which presents the
// maker note as its own directory and derives the facts the camera implied but did not record; and the
// EXIF written with a re-encoded or turned image. What an item takes from a block is libfiles' table,
// which apply_properties maps onto the item.

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

core::orientation metadata_exif::orientation(const std::span<const uint8_t> cs)
{
	const auto stored = libfiles::summarize_exif(cs).orientation;
	return stored ? static_cast<core::orientation>(*stored) : core::orientation::top_left;
}

// A lossless turn changes the pixels' orientation and dimensions and nothing else, so the block is
// patched where those numbers stand: upright in IFD0 and IFD1, the turned size in the Exif and
// Interoperability directories. Nothing else in it changes or moves, so a maker note keeps the
// absolute offsets it relies on. libexif rewrote the whole block, which emptied five of the fixtures'
// Canon maker notes and re-encoded five others.
core::blob metadata_exif::fix_dims(const core::span cs, const int image_width, const int image_height)
{
	core::assert_true(is_exif_signature({cs.data, cs.size}));

	core::blob result(cs.data, cs.data + cs.size);
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

static std::string exif_date_text(const core::date_t date)
{
	const auto d = date.date();
	return std::format("{:04}:{:02}:{:02} {:02}:{:02}:{:02}", d.year, d.month, d.day, d.hour, d.minute, d.second);
}

core::blob metadata_exif::make_exif(const prop::item_metadata_ptr& md)
{
	libfiles::exif_fields f;

	if (md->orientation != core::orientation::none) f.orientation = static_cast<uint16_t>(md->orientation);
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
	core::blob result(exif_signature.begin(), exif_signature.end());
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

metadata_kv_list metadata_exif::to_info(const std::span<const uint8_t> data)
{
	metadata_kv_list result;
	const auto listing = libfiles::list_exif(data);

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
					? std::string(core::utf8_cast(rendered))
					: std::format("{}, {} bytes", vendor.empty() ? std::string_view{"binary"} : vendor, e.bytes.size()));

			row.depth = 1;
			row.shape = std::format("{} x{}, 0x{:04x}", libfiles::exif_type_name(e.type), e.count, e.tag);
			row.id = std::format("exif.{}.{:04x}", ifd_index, e.tag);

			// The bytes stay reachable whether or not they render; the hex listing is built only when the
			// row is opened.
			if (!e.bytes.empty())
			{
				const auto kept = std::min<size_t>(e.bytes.size(), core::max_hex_dump_bytes);
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

			const auto trimmed = core::trim(std::string_view{make->value});
			if (!trimmed.empty() && !is_binary_text(trimmed)) title = std::format("MakerNote ({})", core::utf8_cast(trimmed));
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
			                                readable ? std::string(core::utf8_cast(rendered)) : std::string{"binary"});

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

///////////////////////////////////////////////////////////////////////////////////////////////////
// IPTC metadata. libfiles reads the datasets, and its one table maps captions, keywords, by-lines,
// copyright, places and dates onto an item's properties; this lists the datasets for display.

metadata_kv_list metadata_iptc::to_info(const std::span<const uint8_t> cs)
{
	metadata_kv_list result;

	for (auto& entry : libfiles::list_iptc(cs))
	{
		result.emplace_back(std::move(entry.title), std::move(entry.value));
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// XMP (Extensible Metadata Platform) support. Maps Diffractor's edits into XMP packets, whose data model
// is libfiles', and reads and writes the packets in files with libfiles. What an item takes from a packet
// is libfiles' one table.

namespace xmp_ns = libfiles::xmp::ns;

static core::file_path s_test_raw_source_xmp_path;
static core::blob s_test_raw_source_xmp;
static std::vector<int> s_test_update_failure_sequence;

void metadata_xmp::set_test_raw_source_xmp(const core::file_path path, const core::cspan xmp)
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
	core::bump(df::file_perf.metadata_errors);
	core::log_once(context, message);
}

void metadata_edits::apply(libfiles::xmp::meta& meta) const
{
	if (location_coordinate.has_value())
	{
		const auto position = location_coordinate.value();

		meta.set_property(xmp_ns::exif, "GPSLatitude",
		                 core::utf8_cast2(gps_coordinate::decimal_to_dms_str(position.latitude(), true)));
		meta.set_property(xmp_ns::exif, "GPSLongitude",
		                 core::utf8_cast2(gps_coordinate::decimal_to_dms_str(position.longitude(), false)));
	}


	if (location_place.has_value())
	{
		if (!core::is_empty(location_place.value()))
		{
			meta.set_property(xmp_ns::photoshop, "City", core::utf8_cast2(location_place.value()));
		}
		else
		{
			meta.delete_property(xmp_ns::photoshop, "City");
		}
	}

	if (location_state.has_value())
	{
		if (!core::is_empty(location_state.value()))
		{
			meta.set_property(xmp_ns::photoshop, "State", core::utf8_cast2(location_state.value()));
		}
		else
		{
			meta.delete_property(xmp_ns::photoshop, "State");
		}
	}

	if (location_country.has_value())
	{
		if (!core::is_empty(location_country.value()))
		{
			meta.set_property(xmp_ns::photoshop, "Country", core::utf8_cast2(location_country.value()));
		}
		else
		{
			meta.delete_property(xmp_ns::photoshop, "Country");
		}
	}

	if (copyright_credit.has_value())
	{
		meta.set_property(xmp_ns::photoshop, "Credit", core::utf8_cast2(copyright_credit.value()));
	}

	if (copyright_url.has_value())
	{
		meta.set_property(xmp_ns::xmp_rights, "WebStatement", core::utf8_cast2(copyright_url.value()));
	}

	if (copyright_source.has_value())
	{
		meta.set_property(xmp_ns::photoshop, "Source", core::utf8_cast2(copyright_source.value()));
	}

	if (copyright_notice.has_value())
	{
		meta.set_localized_text(xmp_ns::dc, "rights", "", "x-default", core::utf8_cast2(copyright_notice.value()));
	}

	if (copyright_creator.has_value())
	{
		// dc:creator is an ordered array of names, and that is how it is read back. Writing it as
		// alt-lang text produces a packet Adobe tools will not show.
		meta.delete_property(xmp_ns::dc, "creator");

		if (core::is_empty(copyright_creator.value()))
		{
			meta.set_composite(xmp_ns::dc, "creator", libfiles::xmp::array_is_ordered);
		}
		else
		{
			// Names are separated by punctuation, never by the space inside "Jane Doe".
			for (const auto& part : core::split(copyright_creator.value(), true, core::is_artist_separator))
			{
				const auto name = core::trim(part);
				if (!name.empty())
				{
					meta.append_array_item(xmp_ns::dc, "creator", libfiles::xmp::array_is_ordered, core::utf8_cast2(name));
				}
			}
		}
	}

	if (title.has_value())
	{
		meta.set_localized_text(xmp_ns::dc, "title", "", "x-default", core::utf8_cast2(title.value()));
	}

	if (description.has_value())
	{
		meta.set_localized_text(xmp_ns::dc, "description", "", "x-default", core::utf8_cast2(description.value()));
	}

	if (comment.has_value())
	{
		meta.set_property(xmp_ns::dm, "logComment", core::utf8_cast2(comment.value()));
	}

	if (synopsis.has_value())
	{
		meta.set_property(xmp_ns::dm, "synopsis", core::utf8_cast2(synopsis.value()));
	}

	if (rating.has_value())
	{
		const int r = std::clamp(rating.value(), -1, 5);
		meta.set_property_int(xmp_ns::xmp, "Rating", r);
		// Windows' scale, 1/25/50/75/99, which Explorer buckets: a plain star*20 shows one star too many there.
		meta.set_property_int(xmp_ns::microsoft_photo, "Rating", libfiles::percent_from_stars(r));
	}

	if (label.has_value())
	{
		if (!core::is_empty(label.value()))
		{
			meta.set_property(xmp_ns::xmp, "Label", core::utf8_cast2(label.value()));
		}
		else
		{
			meta.delete_property(xmp_ns::xmp, "Label");
		}
	}

	if (album.has_value())
	{
		meta.set_property(xmp_ns::dm, "album", core::utf8_cast2(album.value()));
	}

	if (album_artist.has_value())
	{
		meta.set_property(xmp_ns::dm, "albumArtist", core::utf8_cast2(album_artist.value()));
	}

	if (genre.has_value())
	{
		meta.set_property(xmp_ns::dm, "genre", core::utf8_cast2(genre.value()));
	}

	if (show.has_value())
	{
		meta.set_property(xmp_ns::dm, "show", core::utf8_cast2(show.value()));
	}

	if (year.has_value() && !date_created.has_value())
	{
		// A bare year and a Created date are the same XMP property, told apart on read by whether it
		// parses as a date. Writing both would leave whichever went last, so the date wins: it is the
		// more specific answer and it still carries the year.
		meta.set_property(xmp_ns::xmp, "CreateDate", core::utf8_cast2(core::to_string(year.value())));
	}

	if (date_created.has_value())
	{
		const auto text = core::utf8_cast2(date_created.value().to_xmp_date());

		// Both, as Original writes both below: xmp:CreateDate is the fourth-ranked Created source, so
		// wherever the file's own DateTimeDigitized survives the write it would outrank the edit and
		// the correction would appear to do nothing.
		meta.set_property(xmp_ns::xmp, "CreateDate", text);
		meta.set_property(xmp_ns::exif, "DateTimeDigitized", text);
	}

	if (date_original.has_value())
	{
		const auto text = core::utf8_cast2(date_original.value().to_xmp_date());

		// Both, because photoshop:DateCreated alone is the lowest-authority capture source and would
		// be outranked by the file's own DateTimeOriginal - the edit would appear to do nothing. The
		// toolkit reconciles exif:DateTimeOriginal back into the embedded EXIF on save.
		meta.set_property(xmp_ns::photoshop, "DateCreated", text);
		meta.set_property(xmp_ns::exif, "DateTimeOriginal", text);
	}

	if (episode.has_value())
	{
		meta.set_property(xmp_ns::dm, "episode", core::utf8_cast2(episode.value().str()));
	}

	if (season.has_value())
	{
		meta.set_property(xmp_ns::dm, "season", core::utf8_cast2(core::to_string(season.value())));
	}

	if (track_num.has_value())
	{
		meta.set_property(xmp_ns::dm, "trackNumber", core::utf8_cast2(track_num.value().str()));
	}

	if (disk_num.has_value())
	{
		meta.set_property(xmp_ns::dm, "discNumber", core::utf8_cast2(disk_num.value().str()));
	}

	if (artist.has_value())
	{
		meta.set_property(xmp_ns::dm, "artist", core::utf8_cast2(artist.value()));
	}

	if (orientation.has_value())
	{
		meta.set_property(xmp_ns::tiff, "Orientation",
		                 core::utf8_cast2(core::to_string(static_cast<int>(orientation.value()))), libfiles::xmp::delete_existing);
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
				tags.add_one(core::cache(str));
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
				meta.append_array_item(xmp_ns::dc, "subject", libfiles::xmp::value_is_array, core::utf8_cast2(tags[i]));
			}

		// Sync exif:XPKeywords (Windows Explorer tag) with dc:subject. Cleared tags are written
		// as an empty value rather than a deleted property: the TIFF export leaves tags that are
		// simply absent from XMP alone, so deleting here would leave the old keywords in place.
		meta.set_property(xmp_ns::exif, "XPKeywords",
		                 tags.is_empty() ? std::string() : core::utf8_cast2(tags.to_string(";", false)));
	}

	if (remove_rating)
	{
		meta.set_property_int(xmp_ns::xmp, "Rating", 0);
		meta.set_property_int(xmp_ns::microsoft_photo, "Rating", 0);
	}
}

// A packet from a JPEG APP1 segment still starts with its signature, which already ends in the NUL
// that terminates the segment header.
static std::string_view packet_text(const std::span<const uint8_t> xmp)
{
	std::string_view text(std::bit_cast<const char*>(xmp.data()), xmp.size());
	if (text.size() > xmp_signature.size() && text.starts_with(xmp_signature)) text.remove_prefix(xmp_signature.size());
	return text;
}

void metadata_xmp::parse(prop::item_metadata& pd, const core::file_path path)
{
	const auto f = platform::blob_from_file(path);
	apply_properties(pd, libfiles::properties_from_xmp(std::span<const uint8_t>(f.data(), f.size())));
}

core::file_path probe_xmp_path(const core::file_path src_path, const std::string_view xmp_name)
{
	if (!xmp_name.empty())
	{
		return src_path.folder().combine_file(xmp_name);
	}

	return src_path.extension(".xmp");
}

bool metadata_xmp::sidecar_holds_metadata(const core::file_path path, const std::string_view xmp_name)
{
	const auto* const ft = files::file_type_from_name(path);
	if (!ft->has_trait(file_traits::embedded_xmp)) return true;
	if (!ft->has_trait(file_traits::raw)) return false;
	return !platform::file_attributes(probe_xmp_path(path, xmp_name)).confirmed_missing();
}

static std::optional<libfiles::packet_reading> read_file_packet(core::file_path path);

// GPano writes these as plain integers. A property the file omits leaves its member zero, which
// prop::panorama_geometry reads as an incomplete declaration and resolves against the pixels.
static void read_panorama_geometry(const libfiles::xmp::meta& xmp, prop::panorama_geometry& g)
{
	const auto read = [&xmp](const char* const name, int& out)
	{
		std::int32_t value = 0;
		if (xmp.get_property_int(xmp_ns::gpano, name, &value)) out = value;
	};

	read("FullPanoWidthPixels", g.full_width);
	read("FullPanoHeightPixels", g.full_height);
	read("CroppedAreaLeftPixels", g.cropped_left);
	read("CroppedAreaTopPixels", g.cropped_top);
	read("CroppedAreaImageWidthPixels", g.cropped_width);
	read("CroppedAreaImageHeightPixels", g.cropped_height);
}

prop::panorama_geometry metadata_xmp::panorama(const core::file_path path)
{
	prop::panorama_geometry result;

	try
	{
		const auto* const ft = files::file_type_from_name(path);

		if (ft->has_trait(file_traits::embedded_xmp))
		{
			libfiles::xmp::meta xmp;
			if (const auto packet = read_file_packet(path))
			{
				if (!packet->packet.empty()) xmp.parse(packet->packet);
			}
			else
			{
				// A HEIF, which libfiles does not update: its packet is the one its scan finds, as
				// read_embedded_source takes it when converting one.
				files ff;
				const auto scanned = ff.scan_file(path, false, ft);
				if (!scanned.success) record_xmp_error(__FUNCTION__, "the panorama metadata could not be read");
				if (!scanned.metadata.xmp.empty()) xmp.parse(packet_text(scanned.metadata.xmp));
			}
			read_panorama_geometry(xmp, result);
			if (result.is_valid()) return result;
		}

		// A stitcher that wrote a sidecar rather than an embedded packet still declared the sphere. The
		// embedded reading is discarded first: read_panorama_geometry fills only what a packet declares,
		// so joining an incomplete embedded declaration to the sidecar's would describe neither file.
		result = {};

		const auto sidecar = platform::blob_from_file(probe_xmp_path(path, {}));

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

// The format libfiles updates a file as, by what the file holds rather than its first bytes alone: an ID3
// tag before a FLAC stream does not make the file MPEG audio, and libfiles cannot update FLAC.
static libfiles::file_format libfiles_update_format(libfiles::reader& r)
{
	return libfiles::info(libfiles::identify(r)).update_format;
}

static libfiles::file_format libfiles_update_format(const core::file_path path)
{
	const auto f = platform::open_file(path, platform::file_open_mode::read);
	if (!f) return libfiles::file_format::unknown;

	platform_file_stream stream(f);
	libfiles::reader r(stream);
	return libfiles_update_format(r);
}

// A format Diffractor reads a packet from but libfiles writes none to - a FLAC stream behind an ID3 tag,
// named as an MP3 - is refused for an edit before anything is planned, whichever way the edit is written.
static void require_updatable(const core::file_path path)
{
	if (!libfiles::can_update(libfiles_update_format(path))) throw app_exception("this file format cannot store the metadata");
}

// The packet a file holds, where libfiles reads and writes it, including compressed JPEG XL boxes.
// Nothing when libfiles does not read the format or the file cannot be opened.
static std::optional<libfiles::packet_reading> read_file_packet(const core::file_path path)
{
	const auto f = platform::open_file(path, platform::file_open_mode::read);
	if (!f) return {};

	platform_file_stream stream(f);
	libfiles::reader r(stream);
	if (r.size() < 16 || !libfiles::can_update(libfiles_update_format(r))) return {};

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

	const auto text = [](const core::cached value) -> std::optional<std::string>
	{
		if (core::is_empty(value)) return {};
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
	if (missing(xmp_ns::dc, "subject") && !core::is_empty(shown.tags)) seed.add_tags = tag_set(shown.tags);

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
		return core::utf8_cast2(*value);
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
		legacy.rating_percent = libfiles::percent_from_stars(r);
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
	if (edits.track_num) legacy.track = core::utf8_cast2(edits.track_num->str());
	if (edits.disk_num) legacy.disc = core::utf8_cast2(edits.disk_num->str());

	// A bare year is the packet's Created date when no full date is given, as apply() writes it.
	if (edits.year && !edits.date_created) legacy.year = edits.year.value();

	return legacy;
}

// What an edit writes: the packet and the fields it sets wherever the format keeps them besides.
// libfiles reads compressed metadata with the same decoder as its scan.
static libfiles::metadata_update update_of(const libfiles::xmp::meta& xmp, const metadata_edits& edits)
{
	libfiles::metadata_update update{&xmp};
	update.legacy = legacy_edits_of(edits, xmp);
	return update;
}

// Patches the file where it lies when nothing it holds has to move - anything new is appended and only
// then referred to, so an interruption leaves a file that reads whole - otherwise writes the updated
// file beside it in one pass and swaps it in, so the file is never left half rewritten.
static void write_with_libfiles(const core::file_path path, const libfiles::metadata_update& update)
{
	auto f = platform::open_file(path, platform::file_open_mode::read);
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

		const auto rw = platform::open_file(path, platform::file_open_mode::read_write);
		if (!rw) throw app_exception("the file could not be opened for writing");

		platform_file_stream file(rw);
		libfiles::write_in_place(file, plan);
		return;
	}

	const auto temp = platform::temp_file(path.extension(), path.folder());
	auto swapped = false;
	const core::scope_exit discard([temp, &swapped] { if (!swapped) platform::delete_file(temp); });

	{
		const auto out = platform::open_file(temp, platform::file_open_mode::create);
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
// does not read - a HEIF being converted - gives the packet diffractor's scan finds.
static void read_embedded_source(const core::file_path src_path, const file_type_ref src_ft, const bool converting,
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
			xmp.append_array_item(xmp_ns::dc, "subject", libfiles::xmp::value_is_array, core::utf8_cast2(shown[i]));
		}
	}
}

// A failure the tests ask for, before anything is read. Action 2 first leaves a byte at the path being
// written, as an update that fails part way would.
static void run_test_update_failure(const core::file_path update_path)
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

// The packet an edit starts from, with the edit applied: the one the source holds, its sidecar's - a
// DNG's too, when it has one - or, for a raw file without a sidecar, the one its scan finds. Where the
// metadata lives is the caller's answer, so the file read is the one the caller decided to write.
static void read_edit_source(const core::file_path src_path, const file_type_ref src_ft, const bool converting,
                             const metadata_edits& edits, const std::string_view src_xmp_name,
                             const bool in_sidecar, libfiles::xmp::meta& xmp)
{
	if (!in_sidecar)
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
			auto f = platform::blob_from_file(path_xmp);

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
				if (!scanned.success && !platform::open_file(src_path, platform::file_open_mode::read))
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
static app_exception file_failure(const std::exception& e, const core::file_path failing_path)
{
	const auto reason = platform::file_write_error(failing_path);
	return app_exception(reason.empty() ? std::string(e.what()) : std::format("{} ({})", reason, e.what()));
}

xmp_update_result metadata_xmp::update(const core::file_path update_path, const core::file_path src_path,
                                       const metadata_edits& edits, const std::string_view src_xmp_name,
                                       const core::file_path dst_xmp_path, const std::optional<bool> src_in_sidecar)
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
		const auto in_sidecar = src_in_sidecar ? *src_in_sidecar : sidecar_holds_metadata(src_path, src_xmp_name);
		// A raw is written where its source keeps its metadata. The source is asked, because a copy being
		// staged has no sidecar of its own yet.
		const auto is_embedded_dst = dst_ft->has_trait(file_traits::embedded_xmp) &&
			!(dst_ft->has_trait(file_traits::raw) && in_sidecar);

		const auto converting = is_embedded_src && is_embedded_dst &&
			libfiles_update_format(src_path) != libfiles_update_format(update_path);

		libfiles::xmp::meta xmp;
		read_edit_source(src_path, src_ft, converting, edits, src_xmp_name, in_sidecar, xmp);

		failing_path = update_path;

		if (is_embedded_dst)
		{
			require_updatable(update_path);
			write_with_libfiles(update_path, update_of(xmp, edits));
			result.success = true;
		}
		else
		{
			failing_path = dst_xmp_path;

			const auto buffer = xmp.serialize({.omit_packet_wrapper = true});

			const auto path_xmp = dst_xmp_path;
			const auto f = platform::open_file(path_xmp, platform::file_open_mode::create);

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
		core::log(__FUNCTION__, e.what());
		throw app_exception(e.what());
	}
	catch (const libfiles::read_error& e)
	{
		core::log(__FUNCTION__, e.what());
		throw file_failure(e, failing_path);
	}
	catch (const libfiles::write_error& e)
	{
		core::log(__FUNCTION__, e.what());
		throw file_failure(e, failing_path);
	}

	return result;
}

core::blob metadata_xmp::encoded_packet(const core::file_path src_path, const core::file_path dst_path,
                                      const metadata_edits& edits, const std::string_view src_xmp_name)
{
	run_test_update_failure(dst_path);

	try
	{
		const auto* const src_ft = files::file_type_from_name(src_path);
		// The destination is not written yet, so its format is the one its name gives.
		const auto dst_format = libfiles::info(libfiles::file_type_from_extension(dst_path.extension())).update_format;
		const auto converting = src_ft->has_trait(file_traits::embedded_xmp) &&
			libfiles_update_format(src_path) != dst_format;

		libfiles::xmp::meta xmp;
		read_edit_source(src_path, src_ft, converting, edits, src_xmp_name,
		                 sidecar_holds_metadata(src_path, src_xmp_name), xmp);
		if (xmp.empty()) return {};

		const auto packet = xmp.serialize();
		const auto* const bytes = std::bit_cast<const uint8_t*>(packet.data());
		return core::blob(bytes, bytes + packet.size());
	}
	catch (const libfiles::xmp::error& e)
	{
		core::log(__FUNCTION__, e.what());
		throw app_exception(e.what());
	}
	catch (const libfiles::read_error& e)
	{
		core::log(__FUNCTION__, e.what());
		throw file_failure(e, src_path);
	}
}

std::optional<xmp_update_result> metadata_xmp::stage_update(const core::file_path stage_path,
                                                            const core::file_path src_path,
                                                            const metadata_edits& edits,
                                                            const std::string_view src_xmp_name)
{
	const auto* const src_ft = files::file_type_from_name(src_path);
	if (!files::file_type_from_name(stage_path)->has_trait(file_traits::embedded_xmp)) return {};
	if (sidecar_holds_metadata(src_path, src_xmp_name)) return {};

	// Held until the stage is written. Windows lets no one write the file meanwhile, so the plan made
	// from it describes the bytes the stage is made from.
	const auto f = platform::open_file(src_path, platform::file_open_mode::read);
	if (!f) return {};

	platform_file_stream source(f);
	libfiles::reader r(source);
	if (r.size() < 16 || !libfiles::can_update(libfiles_update_format(r))) return {};

	run_test_update_failure(stage_path);

	xmp_update_result result;
	auto failing_path = src_path;

	try
	{
		libfiles::xmp::meta xmp;
		read_edit_source(src_path, src_ft, false, edits, src_xmp_name, false, xmp);

		const auto update = update_of(xmp, edits);

		const auto plan = libfiles::plan_update(r, update);
		if (!plan.ok()) throw app_exception(plan.error);

		failing_path = stage_path;

		if (plan.patchable())
		{
			// The file system may copy without moving the bytes - a server-side copy on a share, a
			// block clone on ReFS - so a copy patched where it lies costs least.
			const auto copied = platform::copy_file(src_path, stage_path, true, false);
			if (copied.failed()) throw app_exception(copied.format_error());
			// A copy keeps a read-only source's attribute, which would refuse the write it is made for
			// and then the cleanup of what was left.
			platform::make_file_writable(stage_path);

			write_with_libfiles(stage_path, update);
		}
		else
		{
			const auto out = platform::open_file(stage_path, platform::file_open_mode::create);
			if (!out) throw app_exception("the updated file could not be created");

			platform_file_stream destination(out);
			libfiles::write_updated(r, plan, destination);
		}

		result.success = true;
	}
	catch (const libfiles::xmp::error& e)
	{
		// The packet itself refused the edit, such as a value for what the file holds as an array.
		core::log(__FUNCTION__, e.what());
		throw app_exception(e.what());
	}
	catch (const libfiles::read_error& e)
	{
		core::log(__FUNCTION__, e.what());
		throw file_failure(e, failing_path);
	}
	catch (const libfiles::write_error& e)
	{
		core::log(__FUNCTION__, e.what());
		throw file_failure(e, failing_path);
	}

	return result;
}

bool metadata_xmp::patch(const core::file_path path, const metadata_edits& edits, const std::string_view src_xmp_name)
{
	// The file's own packet is not the one in use, so there is nothing to patch into it.
	if (sidecar_holds_metadata(path, src_xmp_name)) return false;

	run_test_update_failure(path);

	try
	{
		require_updatable(path);

		libfiles::xmp::meta xmp;
		read_edit_source(path, files::file_type_from_name(path), false, edits, src_xmp_name, false, xmp);

		const auto update = update_of(xmp, edits);

		// One handle no one else may open meanwhile, for the plan and the patch: the bytes planned from
		// are the bytes patched.
		const auto f = platform::open_file(path, platform::file_open_mode::read_write);
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
		core::log(__FUNCTION__, e.what());
		throw app_exception(e.what());
	}
	catch (const libfiles::read_error& e)
	{
		core::log(__FUNCTION__, e.what());
		throw file_failure(e, path);
	}
	catch (const libfiles::write_error& e)
	{
		core::log(__FUNCTION__, e.what());
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
		buffer = core::utf8_cast(meta.serialize());
	}
	catch (const libfiles::xmp::error& e)
	{
		core::log(__FUNCTION__, e.what());
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

metadata_kv_list metadata_xmp::to_info(const std::span<const uint8_t> xmp)
{
	bool parsed = false;
	return to_info(xmp, parsed);
}

metadata_kv_list metadata_xmp::to_info(const std::span<const uint8_t> xmp, bool& parsed)
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

			auto& row = result.emplace_back(node.path, core::utf8_cast(node.value));
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

///////////////////////////////////////////////////////////////////////////////////////////////////
// ICC colour profiles, which libfiles lists - the summary, header and tag directory the info pane shows -
// as an item's metadata rows.

metadata_kv_list metadata_icc::to_info(const std::span<const uint8_t> data)
{
	auto rows = libfiles::list_icc(data);
	return to_metadata_kv(rows);
}
