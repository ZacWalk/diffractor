// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Property definitions and metadata formatting. Defines all item property types
// (dimensions, dates, camera info, etc.) and provides formatting functions for display.

#pragma once

#include "model_dates.h"
#include "model_location.h"

enum class icon_index : int;
class file_type;
class file_group;

using file_type_ref = const file_type*;
using file_group_ref = const file_group*;

namespace ui
{
	class surface;
	class image;
	enum class orientation : uint8_t;
}

// Compact category-presence mask used only to reject impossible search candidates.
// It is not a probabilistic hash: shared category bits may produce false positives,
// and the exact search matcher must always decide whether a candidate matches.
struct search_presence_mask
{
	static constexpr uint32_t duplicates = 1 << 30u;
	static constexpr uint32_t crc32c = 1 << 29u;

	static constexpr uint32_t album = 1 << 8u;
	static constexpr uint32_t artist = 1 << 9u;
	static constexpr uint32_t camera = 1 << 10u;
	static constexpr uint32_t camera_settings = 1 << 11u;
	static constexpr uint32_t codec = 1 << 12u;
	static constexpr uint32_t audio_codec = 1 << 13u;
	static constexpr uint32_t created = 1 << 14u;
	static constexpr uint32_t credit = 1 << 15u;
	static constexpr uint32_t duration = 1 << 16u;
	static constexpr uint32_t general = 1 << 17u;
	static constexpr uint32_t genre = 1 << 18u;
	static constexpr uint32_t location = 1 << 19u;
	static constexpr uint32_t rating_label = 1 << 20u;
	static constexpr uint32_t game = 1 << 21u;
	static constexpr uint32_t season = 1 << 22u;
	static constexpr uint32_t tag = 1 << 23u;
	static constexpr uint32_t text = 1 << 24u;
	static constexpr uint32_t year = 1 << 25u;
	static constexpr uint32_t doc_id = 1 << 26u;
	static constexpr uint32_t panorama = 1 << 27u;

	uint32_t types = 0;

	search_presence_mask() noexcept = default;
	~search_presence_mask() noexcept = default;
	search_presence_mask(const search_presence_mask&) noexcept = default;
	search_presence_mask& operator=(const search_presence_mask&) noexcept = default;
	search_presence_mask(search_presence_mask&&) noexcept = default;
	search_presence_mask& operator=(search_presence_mask&&) noexcept = default;

	const search_presence_mask& operator|=(const search_presence_mask& other)
	{
		types |= other.types;
		return *this;
	}

	bool contains_required(const search_presence_mask& required) const
	{
		return (types & required.types) == required.types;
	}
};

constexpr auto label_select_text = "select";
constexpr auto label_second_text = "second";
constexpr auto label_approved_text = "approved";
constexpr auto label_review_text = "review";
constexpr auto label_to_do_text = "to do";

namespace df
{
	struct search_term;
};

// Metadata Constants
namespace prop
{
	struct item_metadata;
	class key;

	// The projection a file's own GPano metadata declares. Persisted in the index, so a value's
	// meaning is fixed once assigned: retire one by leaving its number unused, never by reusing it.
	// `none` is the answer for a file carrying no panorama projection metadata at all, so anything
	// else is the flag - the file says it is a panorama, rather than merely being shaped like one.
	enum class panorama_projection : uint8_t
	{
		none = 0,
		equirectangular = 1,
		cylindrical = 2,
		// GPano panorama properties are present but name no projection this build knows. Still a
		// panorama, and recorded as one, because the declaration is the fact being stored.
		unspecified = 3,
	};

	// Which patch of the sphere a file's pixels cover, in GPano's terms: a notional full panorama
	// of full_width x full_height spanning the whole sphere, and the file's own pixels as a crop
	// out of it. Read from the file at display time rather than indexed, because only the item
	// being looked at needs it and the answer is worth no field of its own.
	struct panorama_geometry
	{
		int full_width = 0;
		int full_height = 0;
		int cropped_left = 0;
		int cropped_top = 0;
		int cropped_width = 0;
		int cropped_height = 0;

		bool operator==(const panorama_geometry&) const noexcept = default;

		// Every member arrives straight from the file as a 32-bit integer, so the two sums are widened:
		// adding two declarations near the top of the range is signed overflow, and a wrapped negative
		// would pass the bound this test exists to enforce.
		bool is_valid() const noexcept
		{
			return full_width > 0 && full_height > 0 && cropped_width > 0 && cropped_height > 0 &&
				cropped_left >= 0 && cropped_top >= 0 &&
				static_cast<int64_t>(cropped_left) + cropped_width <= full_width &&
				static_cast<int64_t>(cropped_top) + cropped_height <= full_height;
		}

		// A writer that declared equirectangular and no crop. The pixels are taken to span the full
		// circle and to sit on the horizon, which is the identity for a true 2:1 sphere and the
		// least wrong guess for anything else - assuming a strip runs pole to pole would bend the
		// horizon of every panorama ever stitched.
		static panorama_geometry assumed(const sizei source) noexcept
		{
			if (source.cx <= 0 || source.cy <= 0) return {};

			const auto full_height = std::max(source.cy, source.cx / 2);

			return {
				source.cx, full_height,
				0, (full_height - source.cy) / 2,
				source.cx, source.cy
			};
		}

		// Fills in whatever the file left out, and rejects a declaration that contradicts the
		// pixels. A crop wider than the full panorama, or a cropped extent that is not the image,
		// describes a different file than the one being drawn.
		static panorama_geometry resolve(const panorama_geometry& declared, const sizei source) noexcept
		{
			if (declared.is_valid() &&
				declared.cropped_width == source.cx && declared.cropped_height == source.cy)
			{
				return declared;
			}

			return assumed(source);
		}
	};

	enum class data_type
	{
		int32 = 1,
		size = 2,
		date = 3,
		float32 = 4,
		string = 5,
		int_pair = 7,
		uint32 = 8,
		string2 = string,
	};

	enum class audio_sample_t
	{
		none = 0,
		unsigned_8bit = 8,
		signed_16bit = 16,
		signed_32bit = 32,
		signed_64bit = 64,
		signed_float = 32 | 1,
		signed_double = 64 | 1,
		unsigned_planar_8bit = 8 | 2,
		signed_planar_16bit = 16 | 2,
		signed_planar_32bit = 32 | 2,
		signed_planar_64bit = 64 | 2,
		planar_float = 32 | 1 | 2,
		planar_double = 64 | 1 | 2,
	};

	namespace style
	{
		enum
		{
			none = 0,
			groupable = 1 << 1,
			sortable = 1 << 2,
			multi_value = 1 << 3,
			fuzzy_search = 1 << 4,
			auto_complete = 1 << 5,
		};
	};

	class key;
	using key_ref = const key*;

	class key
	{
		key(const key& other) = delete;
		key& operator=(const key& other) = delete;
		key(key&& other) noexcept = delete;
		key& operator=(key&& other) = delete;

	public:
		key(uint16_t id, std::string_view sn, std::string_view n, text_t& tx, icon_index i, data_type t,
		    uint32_t f, uint32_t bit);

		uint16_t id = 0;
		icon_index icon = {};
		str::cached short_name = {};
		str::cached name = {};
		text_t& text_key;
		data_type data_type = {};
		uint32_t flags = 0;
		uint32_t search_presence_bit = 0;

		friend bool operator==(const key& lhs, const key& rhs)
		{
			return lhs.id == rhs.id;
		}

		friend bool operator!=(const key& lhs, const key& rhs)
		{
			return !(lhs == rhs);
		}

		constexpr bool operator<(const key& other) const
		{
			return id < other.id;
		}

		constexpr operator key_ref() const
		{
			return this;
		}

		std::string text() const;
	};


	extern key album;
	extern key artist;
	extern key album_artist;
	extern key audio_codec;
	extern key audio_sample_rate;
	extern key audio_sample_rate_full;
	extern key audio_sample_type;
	extern key bitrate;
	extern key camera_manufacturer;
	extern key camera_model;
	extern key audio_channels;
	extern key location_place;
	extern key comment;
	extern key description;
	extern key disk_num;
	extern key composer;
	extern key copyright_credit;
	extern key copyright_source;
	extern key copyright_creator;
	extern key copyright_notice;
	extern key copyright_url;
	extern key location_country;
	extern key created_exif;
	extern key created_digitized;
	extern key created_utc;
	extern key dates_packed;
	extern key dimensions;
	extern key duration;
	extern key encoder;
	extern key encoding_tool;
	extern key episode;
	extern key exposure_time;
	extern key f_number;
	extern key focal_length_35mm_equivalent;
	extern key focal_length;
	extern key pixel_format;
	extern key genre;
	extern key iso_speed;
	extern key altitude;
	extern key gps_speed;
	extern key latitude;
	extern key lens;
	extern key longitude;
	extern key media_category;
	extern key megapixels;
	extern key modified;
	extern key null;
	extern key orientation;
	extern key publisher;
	extern key performer;
	extern key rating;
	extern key season;
	extern key show;
	extern key file_size;
	extern key location_state;
	extern key streams;
	extern key synopsis;
	extern key tag;
	extern key track_num;
	extern key title;
	extern key video_codec;
	extern key year;
	extern key unique_id;
	extern key file_name;
	extern key raw_file_name;
	extern key system;
	extern key game;
	extern key crc32c;
	extern key label;
	extern key doc_id;
	extern key panorama;

	constexpr bool is_null(const std::string_view s)
	{
		return str::is_empty(s);
	}

	constexpr bool is_null(const float n)
	{
		return df::is_zero(n);
	}

	constexpr bool is_null(const int n)
	{
		return n == 0;
	}

	constexpr bool is_null(const uint32_t n)
	{
		return n == 0;
	}

	inline bool is_null(const df::date_t d)
	{
		return !d.is_valid();
	}

	constexpr bool is_null(const sizei s)
	{
		return s.is_empty();
	}

	constexpr bool is_null(const df::xy32 xy)
	{
		return xy.is_empty();
	}

	constexpr bool is_null(const df::xy8 xy)
	{
		return xy.x == 0 || xy.y == 0;
	}

#pragma pack(push, 1)

	struct item_metadata
	{
		item_metadata() noexcept = default;

		item_metadata& operator=(const item_metadata& other) noexcept = default;
		item_metadata& operator=(item_metadata&& other) noexcept = default;

		item_metadata(const item_metadata& other) = default;
		item_metadata(item_metadata&& other) = default;

		sizei dimensions() const { return {width, height}; }

		str::cached album;
		str::cached album_artist;
		str::cached artist;
		str::cached audio_codec;
		str::cached bitrate;
		str::cached camera_manufacturer;
		str::cached camera_model;
		str::cached comment;
		str::cached composer;
		str::cached copyright_creator;
		str::cached copyright_credit;
		str::cached copyright_url;
		str::cached copyright_notice;
		str::cached copyright_source;
		str::cached description;
		str::cached encoder;
		str::cached file_name;
		str::cached genre;
		str::cached lens;
		str::cached location_place;
		str::cached location_country;
		str::cached location_state;
		str::cached performer;
		str::cached pixel_format;
		str::cached publisher;
		str::cached show;
		str::cached synopsis;
		str::cached title;
		str::cached video_codec;
		str::cached raw_file_name;
		str::cached tags;
		str::cached game;
		str::cached system;
		str::cached label;
		str::cached doc_id;

		gps_coordinate coordinate;

		date_pack dates;

		float exposure_time = 0.0f;
		float f_number = 0.0f;
		float focal_length = 0.0f;

		// locations.md 2.8: metres relative to sea level, negative below it, and km/h. Zero means
		// unknown, which reads the same as sea level or standing still and so claims nothing.
		float altitude = 0.0f;
		float gps_speed = 0.0f;

		uint16_t duration = 0;
		uint16_t focal_length_35mm_equivalent = 0;
		uint16_t height = 0;
		uint16_t iso_speed = 0;
		int16_t rating = 0;
		uint16_t audio_channels = 0;
		uint32_t audio_sample_rate = 0;
		uint16_t audio_sample_type = 0;
		uint16_t width = 0;
		uint16_t year = 0;
		uint8_t season = 0;
		panorama_projection panorama = panorama_projection::none;
		ui::orientation orientation = ui::orientation::top_left;
		df::xy8 disk = {0, 0};
		df::xy8 episode = {0, 0};
		df::xy8 track = {0, 0};

		double media_position = 0.0;

		str::cached sidecars;
		str::cached xmp;

		// The one date shown where there is room for one. docs/metadata.md#dates owns the ladder;
		// this is the only place it is implemented.
		df::date_t created() const
		{
			return dates.best();
		}

		search_presence_mask calc_search_presence() const;

		// The file's own declaration, not a guess from its shape. An image merely wide enough to
		// look like a panorama answers false here.
		bool is_panorama() const
		{
			return panorama != panorama_projection::none;
		}

		// docs/zoom.md: what the *display* treats as a panorama, which is a wider question than what
		// the file declares. Failing a declaration, an aspect ratio of at least 2:1 with a long edge
		// of at least 4000 pixels qualifies. Aspect alone would claim every wide screenshot, and the
		// size floor excludes them. Both halves are stored dimensions, so the shape test costs no
		// field and can be retuned without a re-index. `@panorama` stays the declaration alone,
		// because a search is an assertion about the file rather than about how it is drawn.
		bool displays_as_panorama() const
		{
			if (is_panorama()) return true;

			constexpr int min_long_edge = 4000;
			const auto long_edge = std::max<int>(width, height);
			const auto short_edge = std::min<int>(width, height);

			return short_edge > 0 && long_edge >= min_long_edge && long_edge >= short_edge * 2;
		}

		bool has_gps() const
		{
			return coordinate.is_valid();
		}

		std::string format(std::string_view name) const;
	};

#pragma pack(pop)

	using item_metadata_ptr = std::shared_ptr<item_metadata>;
	using item_metadata_aptr = std::atomic<std::shared_ptr<item_metadata>>;
	using item_metadata_const_ptr = std::shared_ptr<const item_metadata>;

	// One populated prose field, in the order the description panel presents them. `duplicate` marks
	// a field repeating text an earlier field already carries, which sidecar round trips make common.
	struct text_field
	{
		std::string_view id; // stable across display languages, unlike name
		std::string_view name;
		str::cached text;
		bool duplicate = false;
	};

	std::vector<text_field> descriptive_fields(const item_metadata& md);

	const key* from_id(uint16_t id);
	const key* from_prefix(std::string_view scope);

	struct prop_scope
	{
		std::string scope;
		const key* type;
	};

	std::vector<prop_scope> search_scopes();
	std::vector<prop_scope> key_scopes();

	int64_t size_bucket(int64_t n);
	double closest_fstop(double fs);
	int64_t exp_round(double d);

	std::string format_f_num(double d);
	std::string format_date(df::date_t d);

	std::string format_duration(int d);
	std::string format_exposure(double d);
	std::string format_focal_length(double d, int filmEquivalent);

	std::string format_gps(double d);
	std::string format_gps(double lat, double lon);
	std::string format_iso(int i);
	std::string format_audio_sample_rate(int v);
	std::string format_audio_sample_rate(uint32_t v);
	std::string format_audio_sample_rate(uint16_t v);
	std::string format_audio_sample_type(audio_sample_t v);
	std::string format_audio_channels(int v);
	std::string format_magnitude(const df::file_size& s);
	std::string format_streams(int s);

	std::string format_rating(int i);
	std::string format_label(std::string_view label);
	std::string format_bit_rate(int64_t i);
	std::string format_dimensions(sizei v);
	std::string format_pixels(sizei v, file_type_ref ft);
	std::string format_video_resolution(sizei v);

	std::string replace_tokens(std::string_view name_template, const item_metadata_const_ptr& md,
	                           std::string_view name, df::date_t created);

	inline double aperture_to_fstop(const double d)
	{
		return std::exp(std::log(2.0) * d / 2.0);
	}
};
