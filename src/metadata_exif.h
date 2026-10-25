// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: EXIF metadata, which libfiles reads, writes and lists: what an item takes from a block, the
// verbose listing, and the EXIF written with a re-encoded or turned image.

#pragma once


namespace prop
{
	struct item_metadata;
	using item_metadata_ptr = std::shared_ptr<item_metadata>;
}


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

// Windows Explorer buckets the percent rating as 1/25/50/75/99. A plain star*20 reads back into
// Diffractor correctly but shows one star too many everywhere else.
constexpr int rating_to_percent(const int stars)
{
	constexpr int percent[] = {0, 1, 25, 50, 75, 99};
	return percent[stars < 0 ? 0 : stars > 5 ? 5 : stars];
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

	// Fills an item from an EXIF block - a TIFF stream - as libfiles reads it. Every caller passes a
	// fresh item, and EXIF comes first: IPTC and XMP override it.
	void parse(prop::item_metadata& pd, df::cspan cs);

	// The orientation an EXIF block gives, or upright when it gives none: libfiles' one rule, which
	// parse, the scan and the JPEG decoder keep too.
	ui::orientation orientation(df::cspan cs);

	// The verbose pane's rows: each directory's entries named and rendered, the maker note decoded, the
	// facts derived from them and the thumbnail, through libfiles' listing.
	metadata_kv_list to_info(df::cspan data);
	// A losslessly turned picture's block, from its "Exif" signature: the orientation upright and the
	// dimensions those given, set where the block holds them, and every other byte as it was.
	df::blob fix_dims(df::span cs, int image_width, int image_height);
	// A fresh EXIF block, from its "Exif" signature, for a picture whose file carries none: libfiles
	// writes it as libexif wrote it.
	df::blob make_exif(const prop::item_metadata_ptr& md);
};
