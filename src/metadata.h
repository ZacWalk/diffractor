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

#pragma once

#include "model_property.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// EXIF metadata, which libfiles reads, writes and lists: the orientation a block gives, the verbose
// listing, and the EXIF written with a re-encoded or turned image.

inline df::blob make_orientation_exif(ui::orientation orientation)
{
	// One IFD and nothing after it. The block used to link an empty IFD1 whose next-IFD offset was
	// cut to two of its four bytes, so every reader following the chain read past the end of it.
	df::blob exif_block = {
		0x49, 0x49,
		0x2A, 0x00,
		0x08, 0x00, 0x00, 0x00, // ifd0 offset
		0x01, 0x00, // ifd0 entrycount
		0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, static_cast<uint8_t>(orientation), 0x00, 0x00, 0x00,
		// orientation
		0x00, 0x00, 0x00, 0x00, // no next ifd
	};

	return exif_block;
}

namespace metadata_exif
{
	template <typename T>
	class rational_t
	{
	public:
		T numerator = 0;
		T denominator = 0;

		rational_t() noexcept = default;
		~rational_t() noexcept = default;
		rational_t(const rational_t&) = default;
		rational_t& operator=(const rational_t&) = default;
		rational_t(rational_t&&) noexcept = default;
		rational_t& operator=(rational_t&&) noexcept = default;

		rational_t(const T n, const T d) noexcept : numerator(n), denominator(d)
		{
		}

		void clear()
		{
			numerator = 0;
			denominator = 0;
		}

		rational_t& operator=(const double other)
		{
			numerator = static_cast<T>(other * 1000.0f);
			denominator = 1000;

			return *this;
		}

		friend bool operator==(const rational_t& lhs, const rational_t& rhs)
		{
			return lhs.numerator == rhs.numerator
				&& lhs.denominator == rhs.denominator;
		}

		friend bool operator!=(const rational_t& lhs, const rational_t& rhs)
		{
			return !(lhs == rhs);
		}

		bool operator<(const rational_t& other) const
		{
			return to_real() < other.to_real();
		}

		double to_real() const
		{
			if (denominator == 0) return 0.0;
			return static_cast<double>(numerator) / static_cast<double>(denominator);
		}

		T mul_div(T m) const
		{
			if (denominator == 0) return 0;
			return df::mul_div(m, numerator, denominator);
		}

		bool is_empty() const
		{
			return numerator == 0 || denominator == 0;
		}

		int round() const
		{
			return df::round(to_real());
		}

		int64_t to_int64() const
		{
			int64_t i = static_cast<uint32_t>(numerator);
			i <<= 32;
			i |= static_cast<uint32_t>(denominator);
			return i;
		}
	};

	using srational32_t = rational_t<int32_t>;
	using urational32_t = rational_t<uint32_t>;

	// The orientation an EXIF block gives, or upright when it gives none: libfiles' one rule, which
	// the scan and the JPEG decoder keep too.
	ui::orientation orientation(std::span<const uint8_t> cs);

	// The verbose pane's rows: each directory's entries named and rendered, the maker note decoded, the
	// facts derived from them and the thumbnail, through libfiles' listing.
	metadata_kv_list to_info(std::span<const uint8_t> data);
	// A losslessly turned picture's block, from its "Exif" signature: the orientation upright and the
	// dimensions those given, set where the block holds them, and every other byte as it was.
	df::blob fix_dims(df::span cs, int image_width, int image_height);
	// A fresh EXIF block, from its "Exif" signature, for a picture whose file carries none: libfiles
	// writes it as libexif wrote it.
	df::blob make_exif(const prop::item_metadata_ptr& md);
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// IPTC metadata's listing for the verbose pane. What an item takes from the block - captions, keywords,
// by-lines, copyright, places and dates - is libfiles' table, which apply_properties maps.

struct metadata_iptc
{
	static metadata_kv_list to_info(std::span<const uint8_t> cs);
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// XMP metadata writing over libfiles' XMP data model: ratings, labels, keywords and the other edits
// Diffractor makes, into packets in files and sidecars. Reading is libfiles' one table.

class metadata_edits;

struct xmp_update_result
{
	bool success = false;
	df::file_path xmp_path;
};

namespace metadata_xmp
{
	// What a sidecar states, onto an item, read by libfiles' one table: each property the packet states
	// replaces the item's. One that cannot be read states nothing.
	void parse(prop::item_metadata& pd, df::file_path path);

	// Which patch of the sphere a panorama declares it holds. Read from the file being displayed
	// rather than from the index: only the item on screen needs it, and reading it here costs no
	// stored field and no re-index. An undeclared or contradictory crop comes back invalid, which
	// the caller resolves against the pixels it actually has.
	prop::panorama_geometry panorama(df::file_path path);

	// Whether a file's metadata is read from and written to its sidecar rather than the file: always for
	// a raw with no packet of its own, and for a DNG when a sidecar is beside it. That is Camera Raw's
	// rule unless it is told to ignore sidecars, and the one the raw scan reads by, so an edit lands
	// where it is read back from; a DNG with no sidecar is written itself, as Lightroom does. A sidecar
	// that cannot be seen is not taken to be absent.
	bool sidecar_holds_metadata(df::file_path path, std::string_view xmp_name);

	// dst_xmp_path is the exact sidecar file to write. The caller stages it next to the media
	// file actually being written so a failure can never touch the live sidecar. src_in_sidecar is
	// sidecar_holds_metadata(src_path, src_xmp_name) when the caller has already asked it and acted on
	// the answer: asked again, a sidecar another application removed meanwhile would send an edit the
	// caller is writing to a sidecar alone into the media file itself.
	xmp_update_result update(df::file_path file_path, df::file_path src_path, const metadata_edits& edits,
	                         std::string_view src_xmp_name, df::file_path dst_xmp_path,
	                         std::optional<bool> src_in_sidecar = {});
	// The packet for a file about to be encoded at dst_path with its packet in it - a JPEG XL, whose boxes
	// libjxl writes as it encodes. Read as update() reads the source, with the edit applied and, where the
	// format changes, what the source shows that its packet lacks. Empty when the packet would hold nothing.
	df::blob encoded_packet(df::file_path src_path, df::file_path dst_path, const metadata_edits& edits,
	                        std::string_view src_xmp_name);
	// Stages a metadata-only edit at stage_path, a new file, with the edit already in it, when the file
	// keeps its packet in a format libfiles writes: a copy patched where the edit fits the space the file
	// holds, otherwise one pass over the source. Nothing, with nothing written, for any other file; the
	// caller copies that and calls update().
	std::optional<xmp_update_result> stage_update(df::file_path stage_path, df::file_path src_path,
	                                              const metadata_edits& edits, std::string_view src_xmp_name);
	// Patches a metadata-only edit into the file where it lies, when it fits the space the file holds:
	// nothing the file holds moves, and libfiles orders the writes so that an interruption leaves a
	// file that reads whole. False, with nothing written, when it does not fit; the caller stages it.
	bool patch(df::file_path path, const metadata_edits& edits, std::string_view src_xmp_name);
	void update(std::string& buffer, const metadata_edits& edits);
	metadata_kv_list to_info(std::span<const uint8_t> xmp);
	// `parsed` is false only when the toolkit could not read the packet; a valid empty packet is parsed.
	metadata_kv_list to_info(std::span<const uint8_t> xmp, bool& parsed);

	void set_test_raw_source_xmp(df::file_path path, df::cspan xmp);
	void set_test_update_failure_sequence(std::vector<int> actions);
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// ICC colour profiles, which libfiles lists: the summary, header and tag directory an item's info pane
// shows.

namespace metadata_icc
{
	metadata_kv_list to_info(std::span<const uint8_t> data);
}
